// SPDX-License-Identifier: GPL-2.0-or-later

#include "translator.h"
#include "config.h"
#include "transliterator.h"

#include <KConfigGroup>
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <QFile>
#include <QGuiApplication>
#include <QNetworkReply>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QTextDocument>
#include <QUrlQuery>
#include <QtQml/QQmlExtensionPlugin>
#include <QtWebEngineQuick>

#include <cstring>
#include <memory>

Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

namespace
{
QString plainText(const QString &html)
{
    QTextDocument document;
    document.setHtml(html);
    return document.toPlainText();
}
}

class TestReply : public QNetworkReply
{
public:
    TestReply(const QNetworkRequest &request, QObject *parent)
        : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly);
    }

    void finish(const QByteArray &data, NetworkError error = NoError)
    {
        payload = data;
        if (error != NoError) {
            setError(error, QStringLiteral("Test network error"));
        }
        setFinished(true);
        Q_EMIT readyRead();
        Q_EMIT finished();
    }

    void abort() override
    {
        finish({}, OperationCanceledError);
    }

    qint64 bytesAvailable() const override
    {
        return payload.size() - offset + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 count = qMin(maxSize, payload.size() - offset);
        if (count <= 0) {
            return -1;
        }
        std::memcpy(data, payload.constData() + offset, count);
        offset += count;
        return count;
    }

private:
    QByteArray payload;
    qint64 offset = 0;
};

class TestNetworkManager : public QNetworkAccessManager
{
public:
    QList<QPointer<TestReply>> replies;
    QList<QByteArray> requestBodies;

protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *outgoingData) override
    {
        auto *reply = new TestReply(request, this);
        replies.append(reply);
        requestBodies.append(outgoingData ? outgoingData->readAll() : QByteArray());
        return reply;
    }
};

class TranslatorTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        Config::self()->setDefaults();
        Config::self()->save();
    }

    void devanagariToIast_data()
    {
        QTest::addColumn<QString>("source");
        QTest::addColumn<QString>("expected");
        QTest::newRow("empty") << QString() << QString();
        QTest::newRow("iast") << QStringLiteral("dharmaḥ – ā ī ū") << QStringLiteral("dharmaḥ – ā ī ū");
        QTest::newRow("sanskrit") << QStringLiteral("श्रीकृष्णः") << QStringLiteral("śrīkṛṣṇaḥ");
        QTest::newRow("mixed") << QStringLiteral("Text: धर्मः\nयोगः १२३ 😀") << QStringLiteral("Text: dharmaḥ\nyogaḥ 123 😀");
        QTest::newRow("virama") << QStringLiteral("तत् त्वम् असि") << QStringLiteral("tat tvam asi");
        QTest::newRow("long") << QStringLiteral("धर्मः ").repeated(1000) << QStringLiteral("dharmaḥ ").repeated(1000);
    }

    void devanagariToIast()
    {
        QFETCH(QString, source);
        QFETCH(QString, expected);
        const auto result = Transliterator::devanagariToIast(source);
        QVERIFY(result.has_value());
        QCOMPARE(*result, expected);
    }

    void rejectsTruncatedTranscription()
    {
        const QString input = QStringLiteral("धर्मः") + QChar::Null + QStringLiteral("योगः");
        QVERIFY(!Transliterator::devanagariToIast(input).has_value());
    }

    void translationOption_data()
    {
        QTest::addColumn<bool>("enabled");
        QTest::addColumn<QString>("source");
        QTest::addColumn<QString>("prefix");
        QTest::newRow("off") << false << QStringLiteral("धर्मः") << QString();
        QTest::newRow("on") << true << QStringLiteral("धर्मः") << QStringLiteral("[dharmaḥ]\n\n");
        QTest::newRow("latin") << true << QStringLiteral("dharmaḥ") << QString();
        QTest::newRow("plain-text") << true << QStringLiteral("<b>धर्मः</b>") << QStringLiteral("[<b>dharmaḥ</b>]\n\n");
        QTest::newRow("multiline") << true << QStringLiteral("धर्मः &  योगः\n  तत्") << QStringLiteral("[dharmaḥ &  yogaḥ\n  tat]\n\n");
    }

    void translationOption()
    {
        QFETCH(bool, enabled);
        QFETCH(QString, source);
        QFETCH(QString, prefix);
        Config::setTranscribeDevanagari(enabled);
        auto *manager = new TestNetworkManager;
        Translator translator(manager, nullptr);
        QSignalSpy ready(&translator, &Translator::translationReady);
        const int id = translator.translate(source);
        QCOMPARE(manager->replies.size(), 1);
        const QUrlQuery query(manager->replies.first()->url());
        QCOMPARE(query.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded), source);
        manager->replies.first()->finish(R"([[["Erster ",""],["zweiter Teil",""]]])");
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(ready.first().at(0).toInt(), id);
        QCOMPARE(plainText(ready.first().at(1).toString()), prefix + QStringLiteral("Erster zweiter Teil"));
        QCOMPARE(ready.first().at(2).toBool(), !prefix.isEmpty());
    }

    void translationFailures_data()
    {
        QTest::addColumn<QString>("failure");
        QTest::newRow("network") << QStringLiteral("network");
        QTest::newRow("missing-key") << QStringLiteral("missing-key");
        QTest::newRow("invalid-json") << QStringLiteral("invalid-json");
        QTest::newRow("empty-response") << QStringLiteral("empty-response");
    }

    void translationFailures()
    {
        QFETCH(QString, failure);
        Config::setTranscribeDevanagari(true);
        if (failure == QLatin1String("missing-key")) {
            Config::setTranslatorEngine(Config::EnumTranslatorEngine::DeepL);
        }
        auto *manager = new TestNetworkManager;
        Translator translator(manager, nullptr);
        QSignalSpy ready(&translator, &Translator::translationReady);
        const int id = translator.translate(QStringLiteral("धर्मः"));
        QVERIFY(ready.isEmpty()); // Even the missing-key error must allow QML to store the ID.
        if (failure == QLatin1String("missing-key")) {
            QVERIFY(manager->replies.isEmpty());
        } else if (failure == QLatin1String("network")) {
            manager->replies.first()->finish({}, QNetworkReply::ConnectionRefusedError);
        } else {
            manager->replies.first()->finish(failure == QLatin1String("invalid-json") ? QByteArray("invalid") : QByteArray("[]"));
        }
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(ready.first().at(0).toInt(), id);
        const QString result = plainText(ready.first().at(1).toString());
        QVERIFY(result.startsWith(QStringLiteral("[dharmaḥ]\n\n")));
        QVERIFY(result.contains(failure == QLatin1String("network") || failure == QLatin1String("missing-key")
                                    ? QStringLiteral("Error")
                                    : QStringLiteral("No translation was returned.")));
    }

    void transcriptionFailureKeepsTranslation()
    {
        Config::setTranscribeDevanagari(true);
        auto *manager = new TestNetworkManager;
        Translator translator(manager, nullptr);
        QSignalSpy ready(&translator, &Translator::translationReady);
        translator.translate(QStringLiteral("धर्मः") + QChar::Null + QStringLiteral("योगः"));
        manager->replies.first()->finish(R"([[["Translated text"]]])");
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(plainText(ready.first().at(1).toString()), QStringLiteral("Devanagari transcription failed.\n\nTranslated text"));
    }

    void overlappingRequestsKeepTheirOwnSettings()
    {
        Config::setTranscribeDevanagari(true);
        auto *manager = new TestNetworkManager;
        Translator translator(manager, nullptr);
        QSignalSpy ready(&translator, &Translator::translationReady);
        const int first = translator.translate(QStringLiteral("धर्मः"));

        Config::setTranslatorEngine(Config::EnumTranslatorEngine::DeepL);
        Config::setDeeplApiKey(QStringLiteral("test:fx"));
        Config::setTranscribeDevanagari(false);
        const int second = translator.translate(QStringLiteral("योगः"));
        QCOMPARE(manager->replies.at(1)->url().host(), QStringLiteral("api-free.deepl.com"));
        const QUrlQuery body(QString::fromUtf8(manager->requestBodies.at(1)));
        QCOMPARE(body.queryItemValue(QStringLiteral("text"), QUrl::FullyDecoded), QStringLiteral("योगः"));

        Config::setTranslatorEngine(Config::EnumTranslatorEngine::Google);
        Config::setGoogleApiKey(QStringLiteral("test"));
        Config::setTranscribeDevanagari(true);
        const int third = translator.translate(QStringLiteral("श्रीकृष्णः"));
        QVERIFY(first != second && second != third);
        manager->replies.at(2)->finish(R"({"data":{"translations":[{"translatedText":"Third"}]}})");
        manager->replies.at(1)->finish(R"({"translations":[{"text":"Second"}]})");
        manager->replies.at(0)->finish(R"([[["First"]]])");
        QTRY_COMPARE(ready.size(), 3);
        QCOMPARE(ready.at(0).at(0).toInt(), third);
        QCOMPARE(plainText(ready.at(0).at(1).toString()), QStringLiteral("[śrīkṛṣṇaḥ]\n\nThird"));
        QCOMPARE(ready.at(1).at(0).toInt(), second);
        QCOMPARE(plainText(ready.at(1).at(1).toString()), QStringLiteral("Second"));
        QCOMPARE(ready.at(2).at(0).toInt(), first);
        QCOMPARE(plainText(ready.at(2).at(1).toString()), QStringLiteral("[dharmaḥ]\n\nFirst"));
    }

    void segmentedTranslationPreservesOrderAndSpacing()
    {
        Config::setTranscribeDevanagari(true);
        auto *manager = new TestNetworkManager;
        Translator translator(manager, nullptr);
        QSignalSpy ready(&translator, &Translator::translationReady);
        const QVariantList segments{
            QVariantMap{{QStringLiteral("text"), QStringLiteral("Hello ")}, {QStringLiteral("lang"), QStringLiteral("en")}},
            QVariantMap{{QStringLiteral("text"), QStringLiteral("धर्मः ")}, {QStringLiteral("lang"), QStringLiteral("sa")}},
            QVariantMap{{QStringLiteral("text"), QStringLiteral("world")}, {QStringLiteral("lang"), QStringLiteral("en")}},
        };
        const int id = translator.translateSegments(segments);
        QCOMPARE(manager->replies.size(), 3);
        QCOMPARE(QUrlQuery(manager->replies.at(0)->url()).queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded), QStringLiteral("Hello"));
        QCOMPARE(QUrlQuery(manager->replies.at(1)->url()).queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded), QStringLiteral("धर्मः"));
        QCOMPARE(QUrlQuery(manager->replies.at(2)->url()).queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded), QStringLiteral("world"));

        manager->replies.at(2)->finish(R"([[["Welt"]]])");
        manager->replies.at(0)->finish(R"([[["Hallo"]]])");
        QVERIFY(ready.isEmpty());
        manager->replies.at(1)->finish(R"([[["Dharma"]]])");
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(ready.first().at(0).toInt(), id);
        QCOMPARE(plainText(ready.first().at(1).toString()), QStringLiteral("[Hello dharmaḥ world]\n\nHallo Dharma Welt"));
        QCOMPARE(ready.first().at(2).toBool(), true);
    }

    void emptySelectionDoesNotRequestTranslation()
    {
        auto *manager = new TestNetworkManager;
        Translator translator(manager, nullptr);
        QSignalSpy ready(&translator, &Translator::translationReady);
        const int id = translator.translate(QStringLiteral(" \n"));
        QVERIFY(manager->replies.isEmpty());
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(ready.first().at(0).toInt(), id);
        QVERIFY(plainText(ready.first().at(1).toString()).isEmpty());
    }

    void viewerShowsTranscription_data()
    {
        QTest::addColumn<QString>("viewer");
        QTest::newRow("epub") << QStringLiteral("EpubViewerPage");
        QTest::newRow("pdf") << QStringLiteral("PdfViewerPage");
    }

    void viewerShowsTranscription()
    {
        QFETCH(QString, viewer);
        Config::setTranscribeDevanagari(true);
        Config::setTranslatorEngine(Config::EnumTranslatorEngine::DeepL);
        // The missing-key path exercises the real singleton without network access.
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        QQmlComponent component(&engine);
        const QString source = QStringLiteral(R"(
            import QtQuick
            import org.kde.kirigami as Kirigami
            import org.kde.arianna
            Kirigami.ApplicationWindow {
                width: 900; height: 700; visible: true
                property var bookListModel: QtObject { signal entryDataUpdated(var entry) }
                function pdfWatermarkFilterMode() { return "off" }
                contextDrawer: Kirigami.ContextDrawer {
                    property string currentTocId: ""
                    property string currentHref: ""
                    property var model: QtObject { function importFromJson(json) {} }
                    function clearCurrentItem() {}
                    function setCurrentItem(id) {}
                    signal goTo(var location)
                    signal removePdfTableOfContentsItem(string tocId)
                    signal editPdfTableOfContentsItem(string tocId, string title)
                }
                pageStack.initialPage: %1 { objectName: "reader"; entry: ({ uniqueIdentifier: "" }) }
            }
        )")
                                   .arg(viewer);
        component.setData(source.toUtf8(), QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        QVERIFY(QTest::qWaitForWindowExposed(quickWindow));
        auto *dialog = window->findChild<QQuickWindow *>(QStringLiteral("translatorDialog"));
        QVERIFY(dialog);
        auto *page = window->findChild<QObject *>(QStringLiteral("reader"));
        auto *result = window->findChild<QObject *>(QStringLiteral("translationResult"));
        QVERIFY(page);
        QVERIFY(result);
        QCOMPARE(result->property("textFormat").toInt(), int(Qt::RichText));
        auto *quickDocument = qvariant_cast<QQuickTextDocument *>(result->property("textDocument"));
        QVERIFY(quickDocument);
        QTextDocument *document = quickDocument->textDocument();
        QVERIFY(document);
        const QString selection = viewer == QLatin1String("EpubViewerPage")
            ? QStringLiteral("backend.dispatch({type: 'selection', payload: {action: 'translate', text: '%1'}})")
            : QStringLiteral("root.translateSelection('%1')");

        QQmlExpression first(qmlContext(result), result, selection.arg(QStringLiteral("धर्मः")));
        first.evaluate();
        QVERIFY2(!first.hasError(), qPrintable(first.error().toString()));
        const int oldId = page->property("translationRequestId").toInt();
        QVERIFY(oldId > 0);
        QQmlExpression second(qmlContext(result), result, selection.arg(QStringLiteral("<b>श्रीकृष्णः</b>")));
        second.evaluate();
        QVERIFY2(!second.hasError(), qPrintable(second.error().toString()));
        QVERIFY(page->property("translationRequestId").toInt() != oldId);
        // The title describes this result even if the option changes in flight.
        Config::setTranscribeDevanagari(false);
        const QString expected = QStringLiteral("[<b>śrīkṛṣṇaḥ</b>]\n\nError: DeepL API key is missing");
        QTRY_COMPARE(document->toPlainText(), expected);
        QCOMPARE(dialog->title(), QStringLiteral("Transcription and Translation"));
        QCOMPARE(page->property("translationRequestId").toInt(), 0);
        QVERIFY(QMetaObject::invokeMethod(result, "selectAll"));
        QCOMPARE(result->property("selectedText").toString().replace(QChar::LineSeparator, QLatin1Char('\n')), expected);

        QTextCursor transcription(document);
        transcription.setPosition(0);
        transcription.setPosition(expected.indexOf(QStringLiteral("\n\n")), QTextCursor::KeepAnchor);
        QVERIFY(transcription.charFormat().fontItalic());
        QVERIFY(transcription.charFormat().fontWeight() < QFont::Bold);
        QTextCursor translation(document);
        translation.setPosition(expected.indexOf(QStringLiteral("\n\n")) + 2);
        translation.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        QVERIFY(!translation.charFormat().fontItalic());

        auto *translator = engine.singletonInstance<Translator *>("org.kde.arianna", "Translator");
        QVERIFY(translator);
        Q_EMIT translator->translationReady(oldId, QStringLiteral("Stale result"), false);
        QCOMPARE(document->toPlainText(), expected);
        QCOMPARE(dialog->title(), QStringLiteral("Transcription and Translation"));

        Config::setTranscribeDevanagari(false);
        first.evaluate();
        QTRY_COMPARE(document->toPlainText(), QStringLiteral("Error: DeepL API key is missing"));
        QCOMPARE(dialog->title(), QStringLiteral("Translation"));

        Config::setTranscribeDevanagari(true);
        QQmlExpression latin(qmlContext(result), result, selection.arg(QStringLiteral("Already Latin")));
        latin.evaluate();
        QVERIFY2(!latin.hasError(), qPrintable(latin.error().toString()));
        QTRY_COMPARE(page->property("translationRequestId").toInt(), 0);
        QCOMPARE(document->toPlainText(), QStringLiteral("Error: DeepL API key is missing"));
        QCOMPARE(dialog->title(), QStringLiteral("Translation"));

        if (viewer == QLatin1String("EpubViewerPage")) {
            Config::setTranscribeDevanagari(false);
            QQmlExpression segmented(qmlContext(result), result,
                QStringLiteral("backend.dispatch({type: 'selection', payload: {action: 'translate', text: 'धर्मः', segments: [{text: 'धर्मः', lang: 'sa'}]}})"));
            segmented.evaluate();
            QVERIFY2(!segmented.hasError(), qPrintable(segmented.error().toString()));
            QTRY_COMPARE(page->property("translationRequestId").toInt(), 0);
            QCOMPARE(document->toPlainText(), QStringLiteral("Error: DeepL API key is missing"));
        }
    }

    void settingsSwitchPersists()
    {
        QVERIFY(!Config::transcribeDevanagari());
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.kirigami as Kirigami
            import org.kde.arianna
            Kirigami.ApplicationWindow {
                width: 800; height: 900
                pageStack.initialPage: SettingsPage {}
            }
        )",
                          QUrl());
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        auto *control = window->findChild<QObject *>(QStringLiteral("transcribeDevanagariSwitch"));
        QVERIFY(control);
        QVERIFY(!control->property("checked").toBool());
        for (bool checked : {true, false}) {
            control->setProperty("checked", checked);
            QVERIFY(QMetaObject::invokeMethod(control, "toggled"));
            QCOMPARE(Config::transcribeDevanagari(), checked);
            const auto saved = KSharedConfig::openConfig(Config::self()->config()->name(), KConfig::SimpleConfig);
            saved->reparseConfiguration();
            QCOMPARE(saved->group(QStringLiteral("TranslatorEngine")).readEntry("transcribeDevanagari", false), checked);
            Config::self()->load();
            QCOMPARE(control->property("checked").toBool(), checked);
        }
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir environment;
    if (!environment.isValid()) {
        return 1;
    }
    const QByteArray path = QFile::encodeName(environment.path());
    qputenv("XDG_DATA_HOME", path);
    qputenv("XDG_CONFIG_HOME", path);
    qputenv("XDG_CACHE_HOME", path);
    qputenv("XDG_RUNTIME_DIR", path);
    qputenv("LANGUAGE", "en");
    QtWebEngineQuick::initialize();
    QGuiApplication application(argc, argv);
    KLocalizedString::setApplicationDomain("arianna");
    TranslatorTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "translatortest.moc"
