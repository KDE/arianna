// SPDX-License-Identifier: GPL-2.0-or-later

#include "speechcontroller.h"
#include <QObject>

#ifndef Q_MOC_RUN
#include "config.h"
#include "sanskritnumbers.h"
#include "fakespeechdispatcher.h"
#include "okularpdfdocument.h"

#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KConfig>
#include <KConfigGroup>
#ifndef Q_MOC_RUN
#include <KZip>
#endif
#include <QFile>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHttpServer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPdfWriter>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTest>
#include <QtQml/QQmlExtensionPlugin>
#include <QtWebEngineQuick>
#include <core/area.h>
#include <algorithm>

Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

namespace
{
QByteArray realSpeechAddress;
QVariant evaluate(QObject *object, const QString &code)
{
    QQmlExpression expression(qmlContext(object), object, code);
    const QVariant result = expression.evaluate();
    if (expression.hasError())
        QTest::qFail(qPrintable(expression.error().toString()), __FILE__, __LINE__);
    return result;
}
QString encoded(const QString &text)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact)) + QStringLiteral("[0]");
}
QStringList packageTexts(const QString &ssml)
{
    QXmlStreamReader xml(ssml);
    QStringList packages;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("p"))
            packages.append(xml.readElementText(QXmlStreamReader::IncludeChildElements));
    }
    if (xml.hasError()) QTest::qFail(qPrintable(xml.errorString()), __FILE__, __LINE__);
    return packages;
}
void clickButton(QQuickItem *button, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QVERIFY(button && button->window());
    QTest::mouseClick(button->window(), Qt::LeftButton, modifiers,
        button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
}
QQuickItem *buttonFor(QQuickItem *parent, QObject *action)
{
    if (parent->isVisible() && parent->property("action").value<QObject *>() == action)
        return parent;
    for (auto *child : parent->childItems())
        if (auto *button = buttonFor(child, action))
            return button;
    return nullptr;
}
}

#endif

class SpeechControllerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void init();
    void nativeSession();
    void indexMarks();
    void iastPronunciation_data();
    void iastPronunciation();
    void germanAbbreviations();
    void sanskritNumbers_data();
    void sanskritNumbers();
    void sanskritNumberPronunciation_data();
    void sanskritNumberPronunciation();
    void referencePronunciation_data();
    void referencePronunciation();
    void sanskritVoiceSelection_data();
    void sanskritVoiceSelection();
    void bengaliVoiceSelection_data();
    void bengaliVoiceSelection();
    void bengaliLanguageSwitching();
    void sanskritPauseAndQueue();
    void realSanskrit();
    void realBengali();
    void continuousQueue();
    void selectionToContinuous();
    void sentenceModeChanges_data();
    void sentenceModeChanges();
    void finishInlineSentence_data();
    void finishInlineSentence();
    void selectionRepeat();
    void preparedSelectionRepeat();
    void preparedSentences();
    void sentencePause_data();
    void sentencePause();
    void sentencePauseChanges();
    void paragraphPause_data();
    void paragraphPause();
    void paragraphPauseChanges();
    void sentencePauseSettings();
    void paragraphPackages();
    void preloadedParagraphs();
    void continuousPreload();
    void preparedInlineMath();
    void realIndexMarks_data();
    void realIndexMarks();
    void realSentenceModeChanges_data();
    void realSentenceModeChanges();
    void realPreparedPause_data();
    void realPreparedPause();
    void realInlineMath();
    void realViewerIndexMarks_data();
    void realViewerIndexMarks();
    void voiceSelectionAndErrors();
    void reconnectAndCancel();
    void viewerControls_data();
    void viewerControls();
    void pdfContinuousReading_data();
    void pdfContinuousReading();
};

#ifndef Q_MOC_RUN
void SpeechControllerTest::init()
{
    Config::setSpeechVoice(QString());
    Config::setSpeechSentencePauseMs(200);
    Config::setSpeechParagraphPauseMs(400);
    Config::setSpeechExpandSanskritNumbers(false);
}

void SpeechControllerTest::nativeSession()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QSignalSpy errors(&speech, &SpeechController::errorOccurred);
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    QSignalSpy finished(&speech, &SpeechController::segmentFinished);
    speech.adjustSpeechRate(30);
    QCOMPARE(speech.speechRate(), 30);
    speech.speakSegments(
        {QVariantMap{{QStringLiteral("text"), QStringLiteral("Hallo <b>Welt</b>!\n.\nGrüße 😀")}, {QStringLiteral("lang"), QStringLiteral("de-DE")}},
         QVariantMap{{QStringLiteral("text"), QStringLiteral("Hello world!")}, {QStringLiteral("lang"), QStringLiteral("en-US")}}});
    QTRY_COMPARE_WITH_TIMEOUT(server.messages().size(), 1, 5000);
    QCOMPARE(server.messages().first().text, QStringLiteral("Hallo <b>Welt</b>!\n.\nGrüße 😀"));
    QCOMPARE(server.messages().first().language, QStringLiteral("de"));
    QVERIFY(server.commands().contains(QStringLiteral("SET SELF RATE 30")));
    QCOMPARE(speech.state(), SpeechController::Speaking);
    QTRY_COMPARE(started.size(), 1);
    QCOMPARE(started.first().at(0).toString(), speech.sessionId());
    QCOMPARE(started.first().at(1).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("0"));
    speech.adjustSpeechRate(10);
    QTRY_VERIFY(server.commands().contains(QStringLiteral("SET SELF RATE 40")));
    speech.pause();
    QTRY_COMPARE(speech.state(), SpeechController::Paused);
    QVERIFY(server.commands().contains(QStringLiteral("PAUSE SELF")));
    // End-of-segment while paused must not start the next language run.
    server.finish(server.messages().last().id);
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(finished.first().at(0).toString(), speech.sessionId());
    QCOMPARE(finished.first().at(1).toString(), QStringLiteral("0"));
    QTest::qWait(100);
    QCOMPARE(server.messages().size(), 1);
    QCOMPARE(speech.state(), SpeechController::Paused);
    speech.resume();
    QTRY_COMPARE(server.messages().size(), 2);
    QTRY_COMPARE(started.size(), 2);
    QCOMPARE(started.last().at(1).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("1"));
    QCOMPARE(server.messages().last().language, QStringLiteral("en"));
    QVERIFY(server.commands().contains(QStringLiteral("RESUME SELF")));
    const auto last = server.messages().last();
    speech.stop();
    QTRY_VERIFY(server.commands().contains(QStringLiteral("CANCEL SELF")));
    QCOMPARE(speech.state(), SpeechController::Idle);
    server.sendOldEnd(last.id, last.client);
    server.sendOldBegin(last.id, last.client);
    speech.speak(QStringLiteral("New selection"), QStringLiteral("en"));
    QTRY_COMPARE(server.messages().size(), 3);
    QTRY_COMPARE(started.size(), 3);
    const auto currentSession = speech.sessionId();
    QCOMPARE(started.last().at(0).toString(), currentSession);
    QVERIFY(started.first().at(0).toString() != currentSession);
    server.sendOldEnd(last.id, last.client);
    server.sendOldBegin(last.id, last.client);
    QTest::qWait(100);
    QCOMPARE(started.size(), 3);
    QCOMPARE(finished.size(), 1);
    QCOMPARE(speech.state(), SpeechController::Speaking);
    server.finish(server.messages().last().id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    QCOMPARE(errors.count(), 0);
    for (const auto &command : server.commands())
        QVERIFY2(!command.contains(QStringLiteral("CANCEL ALL")) && !command.contains(QStringLiteral("PAUSE ALL"))
                     && !command.contains(QStringLiteral("RESUME ALL")),
                 qPrintable(command));
}

void SpeechControllerTest::selectionRepeat()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QSignalSpy changed(&speech, &SpeechController::repeatSelectionChanged);
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    QSignalSpy reached(&speech, &SpeechController::indexMarkReached);
    QSignalSpy refill(&speech, &SpeechController::moreSegmentsRequested);
    QVERIFY(!speech.repeatSelection());
    const QVariantList segments{
        QVariantMap{{QStringLiteral("id"), QStringLiteral("first")}, {QStringLiteral("text"), QStringLiteral("Erster Satz.")},
            {QStringLiteral("lang"), QStringLiteral("de")}, {QStringLiteral("cfi"), QStringLiteral("first-range")},
            {QStringLiteral("marks"), QVariantList{QVariantMap{{QStringLiteral("start"), 0}, {QStringLiteral("length"), 6},
                {QStringLiteral("cfi"), QStringLiteral("first-word")}}}}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("second")}, {QStringLiteral("text"), QStringLiteral("Hello world.")},
            {QStringLiteral("lang"), QStringLiteral("en")}, {QStringLiteral("cfi"), QStringLiteral("second-range")},
            {QStringLiteral("marks"), QVariantList{QVariantMap{{QStringLiteral("start"), 0}, {QStringLiteral("length"), 5},
                {QStringLiteral("cfi"), QStringLiteral("second-word")}}}}}};
    speech.speakSegments(segments);
    const QString session = speech.sessionId();
    QTRY_COMPARE(server.messages().size(), 1);
    // Enabling repeat during playback applies to the captured whole selection.
    speech.setRepeatSelection(true);
    speech.setRepeatSelection(true);
    QCOMPARE(changed.size(), 1);
    for (int i = 0; i < 4; ++i) {
        QTRY_COMPARE(server.messages().size(), i + 1);
        QTRY_COMPARE(started.size(), i + 1);
        const auto segment = segments.at(i % 2).toMap();
        const auto message = server.messages().last();
        QCOMPARE(message.text, segment.value(QStringLiteral("text")).toString());
        QCOMPARE(message.language, segment.value(QStringLiteral("lang")).toString());
        QCOMPARE(started.last().at(1).toMap(), segment);
        QCOMPARE(speech.sessionId(), session);
        server.sendIndexMark(message.id, message.client, message.marks.first().name);
        QTRY_COMPARE(reached.size(), i + 1);
        QCOMPARE(reached.last().at(2).toMap(), segment.value(QStringLiteral("marks")).toList().first().toMap());
        if (i >= 2) QVERIFY(message.marks.first().name != server.messages().at(i - 2).marks.first().name);
        if (i == 2) speech.setRepeatSelection(false);
        server.finish(message.id);
    }
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    QCOMPARE(changed.size(), 2);
    QCOMPARE(server.messages().size(), 4);

    // END while paused at the cycle boundary must wait for Resume.
    speech.setRepeatSelection(true);
    speech.speakSegments({segments.first()});
    QTRY_COMPARE(server.messages().size(), 5);
    const auto paused = server.messages().last();
    speech.pause();
    QTRY_COMPARE(speech.state(), SpeechController::Paused);
    server.finish(paused.id);
    QTest::qWait(100);
    QCOMPARE(speech.state(), SpeechController::Paused);
    QCOMPARE(server.messages().size(), 5);
    speech.resume();
    QTRY_COMPARE(server.messages().size(), 6);
    const auto stopped = server.messages().last();
    speech.stop();
    server.sendOldBegin(stopped.id, stopped.client);
    server.sendOldEnd(stopped.id, stopped.client);
    server.sendIndexMark(stopped.id, stopped.client, stopped.marks.first().name);
    QTest::qWait(100);
    QCOMPARE(speech.state(), SpeechController::Idle);
    QCOMPARE(server.messages().size(), 6);
    QVERIFY(speech.repeatSelection());
    // Starting a new selection replaces the old snapshot; toggling at Idle
    // must not restart a stopped selection.
    speech.speak(QStringLiteral("Neue Auswahl."), QStringLiteral("de"));
    QTRY_COMPARE(server.messages().size(), 7);
    QCOMPARE(server.messages().last().text, QStringLiteral("Neue Auswahl."));
    speech.stop();
    speech.setRepeatSelection(false);
    QTest::qWait(100);
    QCOMPARE(server.messages().size(), 7);

    // A continuous stream never repeats its final batch, including a stream
    // consisting of just one batch, regardless of the selection preference.
    speech.setRepeatSelection(true);
    speech.startReading({segments.first()}, true);
    QTRY_COMPARE(server.messages().size(), 8);
    server.finish(server.messages().last().id);
    QTRY_COMPARE(refill.size(), 1);
    speech.continueReading(speech.sessionId(), {segments.last()}, false);
    QTRY_COMPARE(server.messages().size(), 9);
    server.finish(server.messages().last().id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    speech.startReading({segments.first()}, false);
    QTRY_COMPARE(server.messages().size(), 10);
    server.finish(server.messages().last().id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    QTest::qWait(100);
    QCOMPARE(server.messages().size(), 10);
    QVERIFY(speech.errorString().isEmpty());
}

void SpeechControllerTest::preparedSelectionRepeat()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), false, QStringLiteral("piper"));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    QSignalSpy reached(&speech, &SpeechController::indexMarkReached);
    QVariantList segments;
    for (const auto &text : {QStringLiteral("Erster Satz."), QStringLiteral("Zweiter Satz.")}) {
        segments.append(QVariantMap{{QStringLiteral("id"), text}, {QStringLiteral("text"), text},
            {QStringLiteral("lang"), QStringLiteral("de")}, {QStringLiteral("blockId"), QStringLiteral("paragraph")},
            {QStringLiteral("marks"), QVariantList{QVariantMap{{QStringLiteral("start"), 0}, {QStringLiteral("length"), text.indexOf(QLatin1Char(' '))},
                {QStringLiteral("cfi"), text}}}}});
    }
    speech.setRepeatSelection(true);
    speech.speakSegments(segments);
    const QString session = speech.sessionId();
    for (int cycle = 0; cycle < 2; ++cycle) {
        QTRY_COMPARE(server.messages().size(), cycle + 1);
        QTRY_COMPARE(started.size(), cycle * 2 + 1);
        const auto message = server.messages().last();
        QCOMPARE(message.rawText.count(QStringLiteral("<s>")), 2);
        QCOMPARE(message.rawText.count(QStringLiteral("<break time=\"200ms\"/>")), 1);
        QCOMPARE(started.last().at(1).toMap(), segments.first().toMap());
        QCOMPARE(speech.sessionId(), session);
        if (cycle == 1) {
            QVERIFY(message.marks.first().name != server.messages().first().marks.first().name);
            speech.setRepeatSelection(false);
        }
        for (const auto &mark : message.marks) server.sendIndexMark(message.id, message.client, mark.name);
        QTRY_COMPARE(reached.size(), (cycle + 1) * 2);
        QTRY_COMPARE(started.size(), (cycle + 1) * 2);
        QCOMPARE(started.last().at(1).toMap(), segments.last().toMap());
        server.finish(message.id);
    }
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    QCOMPARE(server.messages().size(), 2);
    QVERIFY(speech.errorString().isEmpty());
}

void SpeechControllerTest::indexMarks()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), true);
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QSignalSpy reached(&speech, &SpeechController::indexMarkReached);
    const QString text = QStringLiteral("Eins & <zwei> 😀 é.");
    const QVariantList marks{
        QVariantMap{{QStringLiteral("start"), 0}, {QStringLiteral("length"), 4}, {QStringLiteral("cfi"), QStringLiteral("first-word")}},
        QVariantMap{{QStringLiteral("start"), -1}, {QStringLiteral("length"), 4}},
        QVariantMap{{QStringLiteral("start"), text.indexOf(QStringLiteral("zwei"))}, {QStringLiteral("length"), 4},
                    {QStringLiteral("cfi"), QStringLiteral("second-word")}},
        QVariantMap{{QStringLiteral("start"), text.indexOf(QStringLiteral("😀")) + 1}, {QStringLiteral("length"), 1}},
        QVariantMap{{QStringLiteral("start"), text.indexOf(QStringLiteral("é"))}, {QStringLiteral("length"), 2},
                    {QStringLiteral("cfi"), QStringLiteral("last-word")}},
    };
    const QVariantMap segment{{QStringLiteral("text"), text}, {QStringLiteral("lang"), QStringLiteral("de")},
                              {QStringLiteral("id"), QStringLiteral("marked")}, {QStringLiteral("marks"), marks}};
    speech.speakSegments({segment});
    QTRY_COMPARE(server.messages().size(), 1);
    const auto first = server.messages().first();
    QCOMPARE(first.module, QStringLiteral("test")); // Honor the configured default before the first matching module.
    QCOMPARE(first.dataMode, QStringLiteral("SSML"));
    QCOMPARE(first.text, text);
    QVERIFY(first.rawText.contains(QStringLiteral("&amp;")));
    QVERIFY(first.rawText.contains(QStringLiteral("&lt;")));
    QCOMPARE(first.marks.size(), 3);
    QCOMPARE(first.marks.at(1).start, text.indexOf(QStringLiteral("zwei")));
    server.sendIndexMark(first.id, first.client, QStringLiteral("unrecognized"));
    server.sendIndexMark(first.id, first.client, first.marks.at(1).name);
    QTRY_COMPARE(reached.size(), 1);
    QCOMPARE(reached.last().at(0).toString(), speech.sessionId());
    QCOMPARE(reached.last().at(1).toString(), QStringLiteral("marked"));
    QCOMPARE(reached.last().at(2).toMap(), marks.at(2).toMap());
    speech.pause();
    QTRY_COMPARE(speech.state(), SpeechController::Paused);
    speech.resume();
    QTRY_COMPARE(speech.state(), SpeechController::Speaking);
    server.sendIndexMark(first.id, first.client, first.marks.last().name);
    QTRY_COMPARE(reached.size(), 2);
    QCOMPARE(reached.last().at(2).toMap(), marks.last().toMap());
    speech.stop();
    speech.speakSegments({segment});
    QTRY_COMPARE(server.messages().size(), 2);
    const auto second = server.messages().last();
    QVERIFY(second.marks.first().name != first.marks.first().name);
    server.sendIndexMark(first.id, first.client, second.marks.first().name);
    server.sendIndexMark(second.id, second.client, first.marks.first().name);
    server.sendIndexMark(second.id, second.client, second.marks.first().name);
    QTRY_COMPARE(reached.size(), 3);
    QCOMPARE(reached.last().at(0).toString(), speech.sessionId());
    QCOMPARE(reached.last().at(2).toMap(), marks.first().toMap());
    server.finish(second.id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    server.sendIndexMark(second.id, second.client, second.marks.last().name);
    speech.speak(QStringLiteral("Plain <text>"), QStringLiteral("de"));
    QTRY_COMPARE(server.messages().size(), 3);
    QCOMPARE(server.messages().last().dataMode, QStringLiteral("TEXT"));
    QCOMPARE(server.messages().last().text, QStringLiteral("Plain <text>"));
    QCOMPARE(reached.size(), 3);
    server.rejectSsml(true);
    speech.speakSegments({segment});
    // Wait for this fallback utterance, rather than the preceding plain text.
    QTRY_COMPARE(server.messages().size(), 4);
    QCOMPARE(server.messages().last().dataMode, QStringLiteral("TEXT"));
    QCOMPARE(server.messages().last().text, text);
    QVERIFY(server.messages().last().marks.isEmpty());
}

void SpeechControllerTest::iastPronunciation_data()
{
    QTest::addColumn<QString>("language");
    QTest::addColumn<QString>("preferredLanguage");
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("expected");
    const QString names = QStringLiteral("Read 😀 Śaṅkarācārya Bhagavad-gītā Śrī Kṛṣṇa.");
    const QString english = QStringLiteral("Read 😀 Shankaracharya Bhagavad-gita Shree Krishna.");
    const QString german = QStringLiteral("Read 😀 Schankaratscharja Bhagavad-gita Schri Krischna.");
    QTest::newRow("en") << QStringLiteral("en-US") << QString() << names << english;
    QTest::newRow("de") << QStringLiteral("de-DE") << QString() << names << german;
    QTest::newRow("en-decomposed") << QStringLiteral("en") << QString() << names.normalized(QString::NormalizationForm_D) << english;
    QTest::newRow("de-decomposed") << QStringLiteral("de") << QString() << names.normalized(QString::NormalizationForm_D) << german;
    QTest::newRow("explicit-en-voice") << QStringLiteral("de") << QStringLiteral("en") << names << english;
    QTest::newRow("explicit-de-voice") << QStringLiteral("en") << QStringLiteral("de") << names << german;
    QTest::newRow("upper-case") << QStringLiteral("en") << QString() << QStringLiteral("KṚṢṆA ŚRĪ.") << QStringLiteral("KRISHNA SHREE.");
    QTest::newRow("other-iast-letters") << QStringLiteral("en") << QString()
        << QStringLiteral("Ṅāna Ḍaṭa ḷ ñāna Ṛṣi saṃskṛta Viṣṇu.")
        << QStringLiteral("Nana Data li nyana Rishi samskrita Vishnu.");
    const QString foreign = QStringLiteral("Café façades Grüße niño Śląsk é.");
    QTest::newRow("foreign-accents") << QStringLiteral("de") << QString() << foreign << foreign;
}

void SpeechControllerTest::iastPronunciation()
{
    QFETCH(QString, language);
    QFETCH(QString, preferredLanguage);
    QFETCH(QString, text);
    QFETCH(QString, expected);
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    if (!preferredLanguage.isEmpty()) {
        speech.refreshVoices();
        QTRY_VERIFY(!speech.voices().isEmpty());
        for (const auto &value : speech.voices()) {
            const auto voice = value.toMap();
            if (voice.value(QStringLiteral("language")).toString() == preferredLanguage)
                Config::setSpeechVoice(voice.value(QStringLiteral("id")).toString());
        }
        QVERIFY(!Config::speechVoice().isEmpty());
    }
    speech.speak(text, language);
    QTRY_COMPARE(server.messages().size(), 1);
    QCOMPARE(server.messages().last().dataMode, QStringLiteral("TEXT"));
    QCOMPARE(server.messages().last().text, expected);
    speech.stop();

    QVariantList marks;
    const auto originals = text.split(QLatin1Char(' '));
    const auto aliases = expected.split(QLatin1Char(' '));
    QCOMPARE(originals.size(), aliases.size());
    QList<qsizetype> expectedStarts;
    qsizetype start = 0;
    qsizetype spokenStart = 0;
    for (qsizetype i = 0; i < originals.size(); ++i) {
        const auto &word = originals.at(i);
        if (!word.isEmpty() && word.front().isLetter()) {
            marks.append(QVariantMap{{QStringLiteral("start"), start}, {QStringLiteral("length"), word.size()},
                                     {QStringLiteral("cfi"), QStringLiteral("original-word-%1").arg(i)}});
            expectedStarts.append(spokenStart);
        }
        start += word.size() + 1;
        spokenStart += aliases.at(i).size() + 1;
    }
    const QVariantMap segment{{QStringLiteral("text"), text}, {QStringLiteral("lang"), language},
                              {QStringLiteral("id"), QStringLiteral("iast")}, {QStringLiteral("marks"), marks}};
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    QSignalSpy reached(&speech, &SpeechController::indexMarkReached);
    speech.speakSegments({segment});
    QTRY_COMPARE(server.messages().size(), 2);
    const auto message = server.messages().last();
    QCOMPARE(message.dataMode, QStringLiteral("SSML"));
    QCOMPARE(message.text, expected);
    QCOMPARE(message.marks.size(), marks.size());
    QTRY_COMPARE(started.size(), 1);
    QCOMPARE(started.first().at(1).toMap(), segment);
    for (qsizetype i = 0; i < marks.size(); ++i) {
        QCOMPARE(message.marks.at(i).start, expectedStarts.at(i));
        server.sendIndexMark(message.id, message.client, message.marks.at(i).name);
        QTRY_COMPARE(reached.size(), i + 1);
        QCOMPARE(reached.last().at(2).toMap(), marks.at(i).toMap());
    }
    server.finish(message.id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    server.rejectSsml(true);
    speech.speakSegments({segment});
    QTRY_COMPARE(server.messages().size(), 3);
    QCOMPARE(server.messages().last().dataMode, QStringLiteral("TEXT"));
    QCOMPARE(server.messages().last().text, expected);
    QVERIFY(server.messages().last().marks.isEmpty());
    QVERIFY(speech.errorString().isEmpty());
}

void SpeechControllerTest::germanAbbreviations()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    const QString source = QStringLiteral("1. Stufe: 2. Stufe, der 3. Tag, das 4. Kapitel, im 5. Band, 21. Stufe, 100. Stufe, 101. Stufe; z.B. (z. B.) gilt das, bzw. auch; u. a. usw. etc. d. h. ca. Nr. Dr. Z. T. i. d. R. u. Ä.");
    const QString expanded = QStringLiteral("erste Stufe: zweite Stufe, der dritte Tag, das vierte Kapitel, im fünften Band, einundzwanzigste Stufe, einhundertste Stufe, einhunderterste Stufe; zum Beispiel (zum Beispiel) gilt das, beziehungsweise auch; unter anderem und so weiter et cetera das heißt circa Nummer Doktor Zum Teil in der Regel und Ähnliches");

    speech.speak(source, QStringLiteral("de"));
    QTRY_COMPARE(server.messages().size(), 1);
    QCOMPARE(server.messages().last().text, expanded);
    speech.stop();

    const QVariantMap segment{{QStringLiteral("id"), QStringLiteral("abbreviations")},
                              {QStringLiteral("lang"), QStringLiteral("de")},
                              {QStringLiteral("text"), source},
                              {QStringLiteral("marks"), QVariantList{
                                  QVariantMap{{QStringLiteral("start"), 0}, {QStringLiteral("length"), 2},
                                              {QStringLiteral("cfi"), QStringLiteral("first-part")}},
                                  QVariantMap{{QStringLiteral("start"), 2}, {QStringLiteral("length"), 2},
                                              {QStringLiteral("cfi"), QStringLiteral("second-part")}}}}};
    QSignalSpy reached(&speech, &SpeechController::indexMarkReached);
    speech.speakSegments({segment});
    QTRY_COMPARE(server.messages().size(), 2);
    QCOMPARE(server.messages().last().dataMode, QStringLiteral("SSML"));
    QCOMPARE(server.messages().last().text, expanded);
    QCOMPARE(server.messages().last().marks.size(), 2);
    QCOMPARE(server.messages().last().marks.first().start, 0);
    server.sendIndexMark(server.messages().last().id, server.messages().last().client, server.messages().last().marks.first().name);
    QTRY_COMPARE(reached.size(), 1);
    QCOMPARE(reached.first().at(2).toMap().value(QStringLiteral("cfi")).toString(), QStringLiteral("first-part"));
    speech.stop();

    const QString initialsText = QStringLiteral("A.\u00a0C.\u00a0Bhaktivedanta Swami Prabhupāda");
    const qsizetype cInitial = initialsText.indexOf(QLatin1Char('C'));
    const QVariantMap initialsSegment{{QStringLiteral("id"), QStringLiteral("ac-initials")},
        {QStringLiteral("lang"), QStringLiteral("de")}, {QStringLiteral("text"), initialsText},
        {QStringLiteral("marks"), QVariantList{
            QVariantMap{{QStringLiteral("start"), 0}, {QStringLiteral("length"), 1}, {QStringLiteral("cfi"), QStringLiteral("a-initial")}},
            QVariantMap{{QStringLiteral("start"), cInitial}, {QStringLiteral("length"), 1}, {QStringLiteral("cfi"), QStringLiteral("c-initial")}}}}};
    speech.speakSegments({initialsSegment});
    QTRY_COMPARE(server.messages().size(), 3);
    const auto initialsMessage = server.messages().last();
    QCOMPARE(initialsMessage.dataMode, QStringLiteral("SSML"));
    QCOMPARE(initialsMessage.text, QStringLiteral("Ah Zeh\u00a0Bhaktivedanta Swami Prabhupada"));
    QCOMPARE(initialsMessage.marks.size(), 2);
    QCOMPARE(initialsMessage.marks.at(0).start, 0);
    QCOMPARE(initialsMessage.marks.at(1).start, 3);
    server.sendIndexMark(initialsMessage.id, initialsMessage.client, initialsMessage.marks.at(1).name);
    QTRY_COMPARE(reached.size(), 2);
    QCOMPARE(reached.last().at(2).toMap().value(QStringLiteral("cfi")).toString(), QStringLiteral("c-initial"));
    speech.stop();

    const QList<QPair<QString, QString>> initialsCases{
        {QStringLiteral("A. C. Bhaktivedanta Swami Prabhupāda"), QStringLiteral("Ah Zeh Bhaktivedanta Swami Prabhupada")},
        {QStringLiteral("A.C. Bhaktivedanta Swami Prabhupāda"), QStringLiteral("Ah Zeh Bhaktivedanta Swami Prabhupada")},
        {QStringLiteral("A.\u00a0C.\u00a0Bhaktivedanta Swami Prabhupāda"), QStringLiteral("Ah Zeh\u00a0Bhaktivedanta Swami Prabhupada")},
        {QStringLiteral("A. C. Prabhupāda"), QStringLiteral("A. C. Prabhupada")},
    };
    qsizetype expectedMessageCount = 3;
    for (const auto &[text, expected] : initialsCases) {
        speech.speak(text, QStringLiteral("de"));
        ++expectedMessageCount;
        QTRY_COMPARE(server.messages().size(), expectedMessageCount);
        QCOMPARE(server.messages().last().text, expected);
        speech.stop();
    }

    server.rejectSsml(true);
    speech.speakSegments({segment});
    ++expectedMessageCount;
    QTRY_COMPARE(server.messages().size(), expectedMessageCount);
    QCOMPARE(server.messages().last().dataMode, QStringLiteral("TEXT"));
    QCOMPARE(server.messages().last().text, expanded);
    speech.stop();

    speech.speak(source, QStringLiteral("en"));
    ++expectedMessageCount;
    QTRY_COMPARE(server.messages().size(), expectedMessageCount);
    QCOMPARE(server.messages().last().text, source);

    speech.speak(QStringLiteral("A. C. Bhaktivedanta Swami Prabhupāda"), QStringLiteral("en"));
    ++expectedMessageCount;
    QTRY_COMPARE(server.messages().size(), expectedMessageCount);
    QCOMPARE(server.messages().last().text, QStringLiteral("A. C. Bhaktivedanta Swami Prabhupada"));
}

void SpeechControllerTest::sanskritNumbers_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("expected");
    QTest::newRow("zero") << QStringLiteral("०") << QStringLiteral("शून्य");
    QTest::newRow("neuter-counting") << QStringLiteral("१ २ ३ ४") << QStringLiteral("एकम् द्वे त्रीणि चत्वारि");
    QTest::newRow("thirteen") << QStringLiteral("१३") << QStringLiteral("त्रयोदश");
    QTest::newRow("ascii") << QStringLiteral("13") << QStringLiteral("त्रयोदश");
    QTest::newRow("verse-markers") << QStringLiteral("॥१३॥") << QStringLiteral("॥त्रयोदश॥");
    QTest::newRow("sentence-punctuation") << QStringLiteral("😀 १३. २३, १००।") << QStringLiteral("😀 त्रयोदश. त्रयोविंशतिः, शतम्।");
    QTest::newRow("irregular-forms") << QStringLiteral("३३ ४४ ५४ ६४ ८२ ८६ ९६ ९९")
        << QStringLiteral("त्रयस्त्रिंशत् चतुश्चत्वारिंशत् चतुःपञ्चाशत् चतुःषष्टिः द्व्यशीतिः षडशीतिः षण्णवतिः नवनवतिः");
    const QStringList unchanged{
        QStringLiteral("१०१ १०८ १००० 2026"), QStringLiteral("०१३ 00"), QStringLiteral("1३ १3"),
        QStringLiteral("१३.५ 13,5 १.१३ 2.13.4"), QStringLiteral("१३:३० 13/14 १३-१४ 13–14"),
        QStringLiteral("-१३ +13 −१३ 13%"), QStringLiteral("vers13 अ१३ १३क x_13 13_"),
        QStringLiteral("١٣ 13३४٥ 13\u0301 १३\u200d"), QStringLiteral("123456789012345678901234567890")
    };
    for (qsizetype index = 0; index < unchanged.size(); ++index)
        QTest::newRow(qPrintable(QStringLiteral("unchanged-%1").arg(index))) << unchanged.at(index) << unchanged.at(index);
}

void SpeechControllerTest::sanskritNumbers()
{
    QFETCH(QString, text);
    QFETCH(QString, expected);
    QCOMPARE(SanskritNumbers::expandForSpeech(text), expected);
    // The source context remains available at SSML marks. Never expand half
    // a numeral or one component of a chapter/verse reference or decimal.
    QCOMPARE(SanskritNumbers::expandForSpeech(u"१३", 0, 1), QStringLiteral("१"));
    QCOMPARE(SanskritNumbers::expandForSpeech(u"१३", 1), QStringLiteral("३"));
    QCOMPARE(SanskritNumbers::expandForSpeech(u"१.१३", 2), QStringLiteral("१३"));
    QCOMPARE(SanskritNumbers::expandForSpeech(u"१३.५", 0, 2), QStringLiteral("१३"));
}

void SpeechControllerTest::sanskritNumberPronunciation_data()
{
    QTest::addColumn<bool>("enabled");
    QTest::addColumn<QString>("language");
    QTest::addColumn<QString>("preferredVoice");
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("expected");
    const QString text = QStringLiteral("😀 श्लोक १३ धर्मः।");
    const QString expected = QStringLiteral("😀 श्लोक त्रयोदश धर्मः।");
    QTest::newRow("enabled") << true << QStringLiteral("sa-Deva") << QString() << text << expected;
    QTest::newRow("disabled") << false << QStringLiteral("sa") << QString() << text << text;
    QTest::newRow("inferred-sanskrit") << true << QStringLiteral("en") << QString() << text << expected;
    QTest::newRow("ascii") << true << QStringLiteral("SA_deva") << QString() << QStringLiteral("13") << QStringLiteral("त्रयोदश");
    QTest::newRow("foreign-voice") << true << QStringLiteral("sa") << QStringLiteral("[\"piper\",\"en-test\"]") << text << text;
    QTest::newRow("explicit-hindi") << true << QStringLiteral("hi") << QStringLiteral("[\"vits\",\"sa_male1\"]") << text << text;
    QTest::newRow("english") << true << QStringLiteral("en") << QString() << QStringLiteral("Chapter 13.") << QStringLiteral("Chapter 13.");
    QTest::newRow("compound-at-word-marks") << true << QStringLiteral("sa") << QString()
        << QStringLiteral("१.१३ 13.5 १३:३० 13/14 13-14") << QStringLiteral("१.१३ 13.5 १३:३० 13/14 13-14");
}

void SpeechControllerTest::sanskritNumberPronunciation()
{
    QFETCH(bool, enabled);
    QFETCH(QString, language);
    QFETCH(QString, preferredVoice);
    QFETCH(QString, text);
    QFETCH(QString, expected);
    Config::setSpeechExpandSanskritNumbers(enabled);
    Config::setSpeechVoice(preferredVoice);
    QTemporaryDir directory;
    FakeSpeechDispatcher server(directory.filePath(QStringLiteral("speech.sock")), true, QStringLiteral("piper"), true);
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(directory.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    speech.speak(text, language);
    QTRY_COMPARE(server.messages().size(), 1);
    QCOMPARE(server.messages().last().dataMode, QStringLiteral("TEXT"));
    QCOMPARE(server.messages().last().text, expected);
    speech.stop();

    QVariantList marks;
    const QRegularExpression words(QStringLiteral("[\\p{L}\\p{M}]+|[0-9०-९]+"));
    auto matches = words.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        marks.append(QVariantMap{{QStringLiteral("start"), match.capturedStart()}, {QStringLiteral("length"), match.capturedLength()},
            {QStringLiteral("cfi"), QStringLiteral("original-number-word-%1").arg(marks.size())}});
    }
    const QVariantMap segment{{QStringLiteral("text"), text}, {QStringLiteral("lang"), language},
        {QStringLiteral("id"), QStringLiteral("numbers")}, {QStringLiteral("marks"), marks}};
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    QSignalSpy reached(&speech, &SpeechController::indexMarkReached);
    speech.speakSegments({segment});
    QTRY_COMPARE(server.messages().size(), 2);
    const auto message = server.messages().last();
    QCOMPARE(message.text, expected);
    QCOMPARE(message.dataMode, QStringLiteral("SSML"));
    QCOMPARE(message.marks.size(), marks.size());
    QTRY_COMPARE(started.size(), 1);
    QCOMPARE(started.first().at(1).toMap(), segment);
    if (text.contains(QStringLiteral("धर्मः")))
        QCOMPARE(message.marks.last().start, expected.indexOf(QStringLiteral("धर्मः")));
    for (qsizetype index = 0; index < marks.size(); ++index) {
        server.sendIndexMark(message.id, message.client, message.marks.at(index).name);
        QTRY_COMPARE(reached.size(), index + 1);
        QCOMPARE(reached.last().at(2).toMap(), marks.at(index).toMap());
    }
    server.finish(message.id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    server.rejectSsml(true);
    speech.speakSegments({segment});
    QTRY_COMPARE(server.messages().size(), 3);
    QCOMPARE(server.messages().last().dataMode, QStringLiteral("TEXT"));
    QCOMPARE(server.messages().last().text, expected);
}

void SpeechControllerTest::referencePronunciation_data()
{
    QTest::addColumn<QString>("language");
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("expected");
    QTest::newRow("de-devanagari") << QStringLiteral("de") << QStringLiteral("भगवद्गीता, Kapitel 2.")
        << QStringLiteral("bhagawadgita, Kapitel 2.");
    QTest::newRow("en-devanagari") << QStringLiteral("en") << QStringLiteral("भगवद्गीता, Chapter 2.")
        << QStringLiteral("bhagavadgita, Chapter 2.");
    QTest::newRow("sa-iast") << QStringLiteral("sa") << QStringLiteral("Bhagavad-gītā, श्लोक २.१।")
        << QStringLiteral("भगवद्-गीता, श्लोक २.१।");
}

void SpeechControllerTest::referencePronunciation()
{
    QFETCH(QString, language);
    QFETCH(QString, text);
    QFETCH(QString, expected);
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), false, QStringLiteral("test"), true);
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QVariantList marks;
    qsizetype offset = 0;
    for (const auto &word : text.split(QLatin1Char(' '))) {
        marks.append(QVariantMap{{QStringLiteral("start"), offset}, {QStringLiteral("length"), word.size()},
            {QStringLiteral("cfi"), QStringLiteral("original-anchor")}});
        offset += word.size() + 1;
    }
    const QVariantMap segment{{QStringLiteral("text"), text}, {QStringLiteral("lang"), language},
        {QStringLiteral("type"), QStringLiteral("reference")}, {QStringLiteral("id"), QStringLiteral("reference")},
        {QStringLiteral("marks"), marks}, {QStringLiteral("cfi"), QStringLiteral("original-anchor")}};
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    QSignalSpy reached(&speech, &SpeechController::indexMarkReached);
    speech.speakSegments({segment});
    QTRY_COMPARE(server.messages().size(), 1);
    const auto message = server.messages().last();
    QCOMPARE(message.text, expected);
    QCOMPARE(message.language, language);
    QCOMPARE(message.dataMode, QStringLiteral("SSML"));
    QCOMPARE(message.marks.size(), marks.size());
    QTRY_COMPARE(started.size(), 1);
    QCOMPARE(started.first().at(1).toMap(), segment);
    for (qsizetype i = 0; i < marks.size(); ++i) {
        server.sendIndexMark(message.id, message.client, message.marks[i].name);
        QTRY_COMPARE(reached.size(), i + 1);
        QCOMPARE(reached.last().at(2).toMap(), marks[i].toMap());
    }
    server.finish(message.id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    server.rejectSsml(true);
    speech.speakSegments({segment});
    QTRY_COMPARE(server.messages().size(), 2);
    QCOMPARE(server.messages().last().dataMode, QStringLiteral("TEXT"));
    QCOMPARE(server.messages().last().text, expected);
    speech.stop();
}

void SpeechControllerTest::sanskritVoiceSelection_data()
{
    QTest::addColumn<QString>("language");
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("preferredVoice");
    QTest::addColumn<QString>("module");
    QTest::addColumn<QString>("spokenLanguage");
    const QString devanagari = QStringLiteral("धर्मः रक्षति।");
    for (const auto &language : {"", "en", "de", "sa", "sa-Deva", "SA_deva"}) {
        QTest::newRow(*language ? language : "unmarked") << QString::fromLatin1(language) << devanagari << QString()
            << QStringLiteral("vits") << QStringLiteral("sa");
    }
    QTest::newRow("mixed-unstructured") << QStringLiteral("en") << QStringLiteral("Read धर्मः today.") << QString()
        << QStringLiteral("piper") << QStringLiteral("en");
    QTest::newRow("explicit-en-voice") << QStringLiteral("sa") << devanagari << QStringLiteral("[\"piper\",\"en-test\"]")
        << QStringLiteral("piper") << QStringLiteral("en");
    QTest::newRow("explicit-sa-male2") << QStringLiteral("en") << devanagari << QStringLiteral("[\"vits\",\"sa_male2\"]")
        << QStringLiteral("vits") << QStringLiteral("sa");
}

void SpeechControllerTest::sanskritVoiceSelection()
{
    QFETCH(QString, language);
    QFETCH(QString, text);
    QFETCH(QString, preferredVoice);
    QFETCH(QString, module);
    QFETCH(QString, spokenLanguage);
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), true, QStringLiteral("piper"), true);
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    Config::setSpeechVoice(preferredVoice);
    SpeechController speech;
    speech.speak(text, language);
    QTRY_COMPARE(server.messages().size(), 1);
    const auto message = server.messages().first();
    QCOMPARE(message.module, module);
    QCOMPARE(message.language, spokenLanguage);
    QCOMPARE(message.text, text);
    if (module == QLatin1String("vits"))
        QCOMPARE(message.voice, preferredVoice.isEmpty() ? QStringLiteral("sa_male1") : QStringLiteral("sa_male2"));
    speech.stop();
    Config::setSpeechVoice(QString());
    QSignalSpy errors(&speech, &SpeechController::errorOccurred);
    // The script alone does not identify Hindi as Sanskrit when Hindi is explicit.
    speech.speak(QStringLiteral("धर्मः"), QStringLiteral("hi"));
    QTRY_COMPARE(errors.size(), 1);
    QCOMPARE(server.messages().size(), 1);
    QVERIFY(speech.errorString().contains(QStringLiteral("'hi'")));
}

void SpeechControllerTest::bengaliVoiceSelection_data()
{
    QTest::addColumn<QString>("language");
    QTest::addColumn<QString>("preferredVoice");
    QTest::addColumn<QString>("module");
    QTest::addColumn<QString>("spokenLanguage");
    QTest::addColumn<QString>("voice");
    for (const auto &language : {"bn", "bn-BD", "bn-IN", "BN_bd", "bn-Beng-BD"}) {
        QTest::newRow(language) << QString::fromLatin1(language) << QString()
            << QStringLiteral("vits") << QStringLiteral("bn") << QStringLiteral("bn_female1");
    }
    QTest::newRow("explicit-male") << QStringLiteral("bn") << QStringLiteral("[\"vits\",\"bn_male1\"]")
        << QStringLiteral("vits") << QStringLiteral("bn") << QStringLiteral("bn_male1");
    QTest::newRow("explicit-female") << QStringLiteral("bn") << QStringLiteral("[\"vits\",\"bn_female1\"]")
        << QStringLiteral("vits") << QStringLiteral("bn") << QStringLiteral("bn_female1");
    QTest::newRow("explicit-english") << QStringLiteral("bn") << QStringLiteral("[\"piper\",\"en-test\"]")
        << QStringLiteral("piper") << QStringLiteral("en") << QStringLiteral("en-test");
}

void SpeechControllerTest::bengaliVoiceSelection()
{
    QFETCH(QString, language);
    QFETCH(QString, preferredVoice);
    QFETCH(QString, module);
    QFETCH(QString, spokenLanguage);
    QFETCH(QString, voice);
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), true, QStringLiteral("piper"), true, true);
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    Config::setSpeechVoice(preferredVoice);
    SpeechController speech;
    QSignalSpy errors(&speech, &SpeechController::errorOccurred);
    const QString text = QStringLiteral("দুষ্ট মন! তুমি কিসের বৈষ্ণব?");
    speech.speak(text, language);
    QTRY_COMPARE(server.messages().size(), 1);
    const auto message = server.messages().first();
    QCOMPARE(message.module, module);
    QCOMPARE(message.language, spokenLanguage);
    QCOMPARE(message.voice, voice);
    QCOMPARE(message.text, text);
    QVERIFY(errors.isEmpty());
    speech.stop();
}

void SpeechControllerTest::bengaliLanguageSwitching()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), true, QStringLiteral("piper"), true, true);
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QSignalSpy reached(&speech, &SpeechController::indexMarkReached);
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    const QStringList languages{QStringLiteral("de"), QStringLiteral("bn-BD"), QStringLiteral("sa-Deva"), QStringLiteral("bn")};
    const QStringList voices{QStringLiteral("de-test"), QStringLiteral("bn_female1"), QStringLiteral("sa_male1"), QStringLiteral("bn_female1")};
    const QStringList texts{QStringLiteral("Guten Tag."), QStringLiteral("নমস্কার।"), QStringLiteral("धर्मः रक्षति।"), QStringLiteral("ভালো থাকুন।")};
    QVariantList segments;
    for (qsizetype i = 0; i < languages.size(); ++i)
        segments.append(QVariantMap{{QStringLiteral("id"), QString::number(i)},
            {QStringLiteral("text"), texts[i]}, {QStringLiteral("lang"), languages[i]},
            {QStringLiteral("cfi"), QStringLiteral("original-range-%1").arg(i)},
            {QStringLiteral("marks"), QVariantList{QVariantMap{{QStringLiteral("start"), 0}, {QStringLiteral("length"), texts[i].size()},
                {QStringLiteral("cfi"), QStringLiteral("original-word-%1").arg(i)}}}}});
    speech.speakSegments(segments);
    for (qsizetype i = 0; i < segments.size(); ++i) {
        QTRY_COMPARE(server.messages().size(), i + 1);
        const auto message = server.messages().last();
        QCOMPARE(message.voice, voices[i]);
        QCOMPARE(message.module, i == 0 ? QStringLiteral("piper") : QStringLiteral("vits"));
        QCOMPARE(message.language, i == 0 ? QStringLiteral("de") : i == 2 ? QStringLiteral("sa") : QStringLiteral("bn"));
        QCOMPARE(message.text, texts[i]);
        QTRY_COMPARE(started.size(), i + 1);
        QCOMPARE(started.last().at(1).toMap(), segments[i].toMap());
        QCOMPARE(message.marks.size(), 1);
        server.sendIndexMark(message.id, message.client, message.marks.first().name);
        QTRY_COMPARE(reached.size(), i + 1);
        QCOMPARE(reached.last().at(2).toMap(), segments[i].toMap().value(QStringLiteral("marks")).toList().first().toMap());
        server.finish(message.id);
    }
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    QVERIFY(speech.errorString().isEmpty());
}

void SpeechControllerTest::sanskritPauseAndQueue()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), false, QStringLiteral("piper"), true);
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QSignalSpy refill(&speech, &SpeechController::moreSegmentsRequested);
    QSignalSpy reached(&speech, &SpeechController::indexMarkReached);
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    const QVariantMap sanskrit{{QStringLiteral("id"), QStringLiteral("sa-original")},
        {QStringLiteral("text"), QStringLiteral("धर्मः रक्षति।")}, {QStringLiteral("lang"), QStringLiteral("en")},
        {QStringLiteral("cfi"), QStringLiteral("original-range")},
        {QStringLiteral("marks"), QVariantList{QVariantMap{{QStringLiteral("start"), 0}, {QStringLiteral("length"), 5},
            {QStringLiteral("cfi"), QStringLiteral("original-word")}}}}};
    const QVariantMap german{{QStringLiteral("text"), QStringLiteral("Guten Tag.")}, {QStringLiteral("lang"), QStringLiteral("de")}};
    speech.startReading({german, sanskrit}, true);
    QTRY_COMPARE(server.messages().size(), 1);
    QCOMPARE(server.messages().last().module, QStringLiteral("piper"));
    server.finish(server.messages().last().id);
    QTRY_COMPARE(server.messages().size(), 2);
    const auto interrupted = server.messages().last();
    QCOMPARE(interrupted.module, QStringLiteral("vits"));
    QTRY_COMPARE(started.size(), 2);
    QCOMPARE(started.last().at(1).toMap(), sanskrit);
    speech.pause();
    QTRY_COMPARE(speech.state(), SpeechController::Paused);
    QVERIFY(server.commands().contains(QStringLiteral("CANCEL SELF")));
    QVERIFY(!server.commands().contains(QStringLiteral("PAUSE SELF")));
    server.sendOldEnd(interrupted.id, interrupted.client);
    server.sendIndexMark(interrupted.id, interrupted.client, interrupted.marks.first().name);
    QTest::qWait(100);
    QCOMPARE(reached.size(), 0);
    speech.adjustSpeechRate(10);
    speech.resume();
    QTRY_COMPARE(server.messages().size(), 3);
    const auto resumed = server.messages().last();
    QCOMPARE(resumed.text, interrupted.text);
    QCOMPARE(resumed.module, QStringLiteral("vits"));
    QVERIFY(resumed.marks.first().name != interrupted.marks.first().name);
    server.sendIndexMark(resumed.id, resumed.client, resumed.marks.first().name);
    QTRY_COMPARE(reached.size(), 1);
    QCOMPARE(reached.first().at(2).toMap(), sanskrit.value(QStringLiteral("marks")).toList().first().toMap());
    server.finish(resumed.id);
    QTRY_COMPARE(refill.size(), 1);
    speech.continueReading(speech.sessionId(), {sanskrit, german}, false);
    QTRY_COMPARE(server.messages().size(), 4);
    QCOMPARE(server.messages().last().module, QStringLiteral("vits"));
    server.finish(server.messages().last().id);
    QTRY_COMPARE(server.messages().size(), 5);
    QCOMPARE(server.messages().last().module, QStringLiteral("piper"));
    server.finish(server.messages().last().id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    QVERIFY(speech.errorString().isEmpty());
}

void SpeechControllerTest::realSanskrit()
{
    if (!qEnvironmentVariableIntValue("ARIANNA_TEST_SANSKRIT"))
        QSKIP("Set ARIANNA_TEST_SANSKRIT=1 to test the native Sanskrit vits module.");
    if (realSpeechAddress.isEmpty()) qunsetenv("SPEECHD_ADDRESS");
    else qputenv("SPEECHD_ADDRESS", realSpeechAddress);
    SpeechController speech;
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    QSignalSpy reached(&speech, &SpeechController::indexMarkReached);
    QVariantList segments;
    for (const auto &language : {QStringLiteral("en"), QStringLiteral("sa-Deva")}) {
        const QString text = QStringLiteral("धर्मः रक्षति।");
        segments.append(QVariantMap{{QStringLiteral("text"), text}, {QStringLiteral("lang"), language},
            {QStringLiteral("id"), language}, {QStringLiteral("marks"), QVariantList{
                QVariantMap{{QStringLiteral("start"), 0}, {QStringLiteral("length"), 5}, {QStringLiteral("cfi"), QStringLiteral("word-one")}},
                QVariantMap{{QStringLiteral("start"), 6}, {QStringLiteral("length"), text.size() - 7}, {QStringLiteral("cfi"), QStringLiteral("word-two")}}}}});
    }
    speech.speakSegments(segments);
    QTRY_VERIFY_WITH_TIMEOUT(speech.state() == SpeechController::Idle || !speech.errorString().isEmpty(), 45000);
    QVERIFY2(speech.errorString().isEmpty(), qPrintable(speech.errorString()));
    QCOMPARE(started.size(), 2);
    QCOMPARE(reached.size(), 4);
    for (qsizetype i = 0; i < segments.size(); ++i) {
        const auto segment = segments.at(i).toMap();
        QCOMPARE(started.at(i).at(1).toMap(), segment);
        const auto marks = segment.value(QStringLiteral("marks")).toList();
        for (qsizetype j = 0; j < marks.size(); ++j)
            QCOMPARE(reached.at(2 * i + j).at(2).toMap(), marks.at(j).toMap());
    }
    qInfo() << "Real Sanskrit index marks: automatic Devanagari and sa-Deva voice selection";
}

void SpeechControllerTest::realBengali()
{
    if (!qEnvironmentVariableIntValue("ARIANNA_TEST_BENGALI"))
        QSKIP("Set ARIANNA_TEST_BENGALI=1 to test the native Bengali vits voices.");
    if (realSpeechAddress.isEmpty()) qunsetenv("SPEECHD_ADDRESS");
    else qputenv("SPEECHD_ADDRESS", realSpeechAddress);
    SpeechController speech;
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    QSignalSpy reached(&speech, &SpeechController::indexMarkReached);
    const QString text = QStringLiteral("নমস্কার, সবাই ভালো থাকুন।");
    const QVariantList marks{QVariantMap{{QStringLiteral("start"), 0}, {QStringLiteral("length"), 7}, {QStringLiteral("cfi"), QStringLiteral("bn-first")}},
        QVariantMap{{QStringLiteral("start"), 9}, {QStringLiteral("length"), 4}, {QStringLiteral("cfi"), QStringLiteral("bn-second")}}};
    const QVariantMap segment{{QStringLiteral("id"), QStringLiteral("bn-live")},
        {QStringLiteral("text"), text}, {QStringLiteral("lang"), QStringLiteral("bn-BD")},
        {QStringLiteral("marks"), marks}, {QStringLiteral("cfi"), QStringLiteral("bn-original-range")}};
    for (const QString &voice : {QString(), QStringLiteral("[\"vits\",\"bn_male1\"]")}) {
        Config::setSpeechVoice(voice);
        started.clear();
        reached.clear();
        speech.speakSegments({segment});
        QTRY_VERIFY_WITH_TIMEOUT(speech.state() == SpeechController::Idle || !speech.errorString().isEmpty(), 45000);
        QVERIFY2(speech.errorString().isEmpty(), qPrintable(speech.errorString()));
        QCOMPARE(started.size(), 1);
        QCOMPARE(started.first().at(1).toMap(), segment);
        QCOMPARE(reached.size(), marks.size());
        for (qsizetype i = 0; i < marks.size(); ++i)
            QCOMPARE(reached[i].at(2).toMap(), marks[i].toMap());
    }
    qInfo() << "Real Bengali index marks: automatic bn-BD routing and explicit male voice";
}

void SpeechControllerTest::continuousQueue()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QSignalSpy refill(&speech, &SpeechController::moreSegmentsRequested);
    const QVariantList first{QVariantMap{{QStringLiteral("text"), QStringLiteral("First sentence.")}, {QStringLiteral("lang"), QStringLiteral("en")}}};
    const QVariantList second{QVariantMap{{QStringLiteral("text"), QStringLiteral("Zweiter Satz.")}, {QStringLiteral("lang"), QStringLiteral("de")}}};
    speech.startReading(first, true);
    const QString session = speech.sessionId();
    QTRY_COMPARE(server.messages().size(), 1);
    server.finish(server.messages().last().id);
    QTRY_COMPARE(refill.size(), 1);
    QCOMPARE(refill.first().first().toString(), session);
    QCOMPARE(speech.state(), SpeechController::Speaking);
    speech.pause();
    QTRY_COMPARE(speech.state(), SpeechController::Paused);
    speech.adjustSpeechRate(20);
    speech.continueReading(session, second, true);
    QTest::qWait(100);
    QCOMPARE(server.messages().size(), 1);
    speech.resume();
    QTRY_COMPARE(server.messages().size(), 2);
    QCOMPARE(speech.sessionId(), session);
    QCOMPARE(server.messages().last().language, QStringLiteral("de"));
    QVERIFY(server.commands().contains(QStringLiteral("SET SELF RATE 20")));
    server.finish(server.messages().last().id);
    QTRY_COMPARE(refill.size(), 2);
    speech.continueReading(QStringLiteral("stale"), first, false);
    speech.continueReading(session, {}, true); // An empty section can be skipped.
    QTRY_COMPARE(refill.size(), 3);
    speech.stop();
    speech.continueReading(session, first, false);
    QTest::qWait(100);
    QCOMPARE(server.messages().size(), 2);
    QCOMPARE(speech.state(), SpeechController::Idle);
    speech.startReading({}, true);
    QTRY_COMPARE(refill.size(), 4);
    speech.continueReading(speech.sessionId(), first, false);
    QTRY_COMPARE(server.messages().size(), 3);
    server.finish(server.messages().last().id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    QCOMPARE(refill.size(), 4);
}

void SpeechControllerTest::selectionToContinuous()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), false, QStringLiteral("piper"));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QSignalSpy extended(&speech, &SpeechController::readingExtended);
    QSignalSpy refill(&speech, &SpeechController::moreSegmentsRequested);
    const auto segment = [](const QString &id) {
        return QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("text"), QString(id + QStringLiteral(" Satz."))},
            {QStringLiteral("lang"), QStringLiteral("de")}, {QStringLiteral("cfi"), QString(id + QStringLiteral("-range"))},
            {QStringLiteral("blockId"), QStringLiteral("paragraph")}, {QStringLiteral("blockTag"), QStringLiteral("p")},
            {QStringLiteral("marks"), QVariantList{QVariantMap{{QStringLiteral("start"), 0}, {QStringLiteral("length"), id.size()}}}}};
    };
    const auto first = segment(QStringLiteral("Erster"));
    const auto second = segment(QStringLiteral("Zweiter"));
    const auto beyond = segment(QStringLiteral("Danach"));
    speech.setRepeatSelection(true);
    speech.speakSegments({first, second});
    QTRY_COMPARE(server.messages().size(), 1);
    const auto selected = server.messages().first();
    const QString session = speech.sessionId();
    speech.setRepeatSelection(false);
    speech.extendReading(QStringLiteral("stream-1"));
    QTRY_COMPARE(extended.size(), 1);
    QCOMPARE(extended.first().at(0).toString(), session);
    QCOMPARE(extended.first().at(1).toString(), QStringLiteral("stream-1"));
    QCOMPARE(extended.first().at(2).toMap(), second);
    QCOMPARE(speech.sessionId(), session);
    QVERIFY(!server.commands().contains(QStringLiteral("CANCEL SELF")));
    speech.continueReading(session, {beyond}, false);
    QTest::qWait(100);
    QCOMPARE(server.messages().size(), 1);
    server.finish(selected.id);
    QTRY_COMPARE(server.messages().size(), 2);
    QCOMPARE(server.messages().last().text, QStringLiteral("Danach Satz."));
    server.finish(server.messages().last().id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    QCOMPARE(refill.size(), 0);
    speech.extendReading(QStringLiteral("stopped"));
    QTest::qWait(50);
    QCOMPARE(extended.size(), 1);
    // Promoting a paused selection after its END must continue after the
    // completed sentence, rather than losing the anchor or repeating it.
    speech.setRepeatSelection(true);
    speech.speakSegments({first});
    QTRY_COMPARE(server.messages().size(), 3);
    speech.pause();
    QTRY_COMPARE(speech.state(), SpeechController::Paused);
    server.finish(server.messages().last().id);
    QTest::qWait(100);
    QCOMPARE(speech.state(), SpeechController::Paused);
    speech.setRepeatSelection(false);
    speech.extendReading(QStringLiteral("stream-2"));
    QTRY_COMPARE(extended.size(), 2);
    QCOMPARE(extended.last().at(2).toMap(), first);
    speech.continueReading(speech.sessionId(), {beyond}, false);
    QTest::qWait(50);
    QCOMPARE(server.messages().size(), 3);
    speech.resume();
    QTRY_COMPARE(server.messages().size(), 4);
    QCOMPARE(server.messages().last().text, QStringLiteral("Danach Satz."));
    server.finish(server.messages().last().id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
}

void SpeechControllerTest::sentenceModeChanges_data()
{
    QTest::addColumn<bool>("prepared");
    QTest::addColumn<bool>("repeat");
    QTest::addColumn<bool>("paused");
    for (bool prepared : {false, true})
        for (bool repeat : {false, true})
            for (bool paused : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(prepared ? "piper" : "per-sentence",
                    repeat ? "repeat" : "once", paused ? "paused" : "speaking"))) << prepared << repeat << paused;
}

void SpeechControllerTest::sentenceModeChanges()
{
    QFETCH(bool, prepared);
    QFETCH(bool, repeat);
    QFETCH(bool, paused);
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), false, prepared ? QStringLiteral("piper") : QStringLiteral("test"));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    QSignalSpy refill(&speech, &SpeechController::moreSegmentsRequested);
    const auto segment = [](const QString &id) {
        return QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("text"), QString(id + QStringLiteral(" Satz."))},
            {QStringLiteral("lang"), QStringLiteral("de")}, {QStringLiteral("blockId"), QStringLiteral("paragraph")},
            {QStringLiteral("blockTag"), QStringLiteral("p")},
            {QStringLiteral("marks"), QVariantList{QVariantMap{{QStringLiteral("start"), 0}, {QStringLiteral("length"), id.size()}}}}};
    };
    const auto first = segment(QStringLiteral("Erster"));
    const auto second = segment(QStringLiteral("Zweiter"));
    const auto third = segment(QStringLiteral("Dritter"));
    speech.startReading({first, second, third}, true);
    const QString session = speech.sessionId();
    QTRY_COMPARE(server.messages().size(), 1);
    const auto batch = server.messages().first();
    if (prepared) {
        QTRY_COMPARE(refill.size(), 1);
        speech.continueReading(session, {segment(QStringLiteral("Vorgeladen"))}, true);
        server.sendIndexMark(batch.id, batch.client, batch.marks.at(2).name);
    } else {
        server.finish(batch.id);
        QTRY_COMPARE(server.messages().size(), 2);
    }
    QTRY_COMPARE(started.size(), 2);
    QCOMPARE(started.last().at(1).toMap(), second);
    if (paused) {
        speech.pause();
        QTRY_COMPARE(speech.state(), SpeechController::Paused);
    }
    const int before = server.messages().size();
    speech.finishSentence(repeat);
    speech.continueReading(session, {segment(QStringLiteral("Verspaetet"))}, true);
    QTest::qWait(100);
    QCOMPARE(server.messages().size(), before);
    QCOMPARE(speech.sessionId(), session);
    if (paused) {
        QCOMPARE(speech.state(), SpeechController::Paused);
        speech.resume();
        if (prepared) {
            QTRY_COMPARE(server.messages().size(), before + 1);
            QCOMPARE(server.messages().last().text, QStringLiteral("Zweiter Satz."));
        }
    }
    const int completed = server.messages().size();
    const auto current = server.messages().last();
    if (prepared && !paused) server.sendIndexMark(batch.id, batch.client, batch.marks.at(4).name);
    else server.finish(current.id);
    if (repeat) {
        QTRY_COMPARE(server.messages().size(), completed + 1);
        QCOMPARE(server.messages().last().text, QStringLiteral("Zweiter Satz."));
        const auto repeated = server.messages().last();
        server.sendOldEnd(batch.id, batch.client);
        QTest::qWait(50);
        QCOMPARE(server.messages().size(), completed + 1);
        speech.setRepeatSelection(false);
        server.finish(repeated.id);
    }
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    for (const auto &event : started) QVERIFY(event.at(1).toMap().value(QStringLiteral("id")) != third.value(QStringLiteral("id")));
    QCOMPARE(refill.size(), prepared ? 1 : 0);
    speech.resume();
    speech.continueReading(session, {third}, false);
    QTest::qWait(50);
    QCOMPARE(speech.state(), SpeechController::Idle);
}

void SpeechControllerTest::finishInlineSentence_data()
{
    QTest::addColumn<bool>("prepared");
    QTest::newRow("piper") << true;
    QTest::newRow("per-segment") << false;
}

void SpeechControllerTest::finishInlineSentence()
{
    QFETCH(bool, prepared);
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), false, prepared ? QStringLiteral("piper") : QStringLiteral("test"));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    const auto part = [](const QString &id, const QString &text, const QString &sentenceId) {
        return QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("text"), text}, {QStringLiteral("lang"), QStringLiteral("de")},
            {QStringLiteral("sentenceId"), sentenceId}, {QStringLiteral("blockId"), QStringLiteral("paragraph")},
            {QStringLiteral("blockTag"), QStringLiteral("p")}, {QStringLiteral("href"), QStringLiteral("chapter")}};
    };
    const auto before = part(QStringLiteral("before"), QStringLiteral("Vor "), QStringLiteral("one"));
    auto formula = part(QStringLiteral("formula"), QStringLiteral("x"), QStringLiteral("one"));
    formula.insert(QStringLiteral("type"), QStringLiteral("math"));
    formula.insert(QStringLiteral("inline"), true);
    const auto after = part(QStringLiteral("after"), QStringLiteral(" nach."), QStringLiteral("one"));
    const auto later = part(QStringLiteral("later"), QStringLiteral("Danach."), QStringLiteral("two"));
    speech.startReading({before, formula, after, later}, true);
    QTRY_COMPARE(server.messages().size(), 1);
    const auto batch = server.messages().first();
    if (prepared) {
        QCOMPARE(batch.marks.size(), 4);
        server.sendIndexMark(batch.id, batch.client, batch.marks.at(1).name);
    } else {
        server.finish(batch.id);
        QTRY_COMPARE(server.messages().size(), 2);
    }
    QTRY_COMPARE(started.size(), 2);
    QCOMPARE(started.last().at(1).toMap(), formula);
    speech.finishSentence(true);
    QTest::qWait(50);
    if (prepared) server.sendIndexMark(batch.id, batch.client, batch.marks.at(2).name);
    else {
        server.finish(server.messages().last().id);
        QTRY_COMPARE(server.messages().size(), 3);
    }
    QTRY_COMPARE(started.size(), 3);
    QCOMPARE(started.last().at(1).toMap(), after);
    if (prepared) server.sendIndexMark(batch.id, batch.client, batch.marks.at(3).name);
    else server.finish(server.messages().last().id);
    QTRY_COMPARE(server.messages().size(), prepared ? 2 : 4);
    // Repeat the entire sentence, including prose before the current formula.
    const auto repeated = server.messages().last();
    QCOMPARE(repeated.text, prepared ? QStringLiteral("Vor x nach.") : QStringLiteral("Vor "));
    speech.setRepeatSelection(false);
    if (!prepared) {
        server.finish(repeated.id);
        QTRY_COMPARE(server.messages().size(), 5);
        QCOMPARE(server.messages().last().text, QStringLiteral("x"));
        server.finish(server.messages().last().id);
        QTRY_COMPARE(server.messages().size(), 6);
        QCOMPARE(server.messages().last().text, QStringLiteral(" nach."));
    }
    server.finish(server.messages().last().id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    for (const auto &event : started) QVERIFY(event.at(1).toMap() != later);
}

void SpeechControllerTest::preparedSentences()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), false, QStringLiteral("piper"));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    QSignalSpy finished(&speech, &SpeechController::segmentFinished);
    QSignalSpy reached(&speech, &SpeechController::indexMarkReached);
    const auto sentence = [](const QString &id, const QString &text, const QString &lang = QStringLiteral("de")) {
        return QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("text"), text}, {QStringLiteral("lang"), lang},
                           {QStringLiteral("href"), QStringLiteral("chapter.xhtml")},
                           {QStringLiteral("blockId"), QStringLiteral("paragraph-one")},
                           {QStringLiteral("blockTag"), QStringLiteral("p")},
                           {QStringLiteral("marks"), QVariantList{QVariantMap{{QStringLiteral("start"), 0},
                               {QStringLiteral("length"), text.indexOf(QLatin1Char(' '))}, {QStringLiteral("cfi"), QString(id + QStringLiteral("-word"))}}}}};
    };
    const auto first = sentence(QStringLiteral("first"), QStringLiteral("Erster Satz."));
    const auto second = sentence(QStringLiteral("second"), QStringLiteral("Zweiter Satz."));
    const auto english = sentence(QStringLiteral("english"), QStringLiteral("Third sentence."), QStringLiteral("en"));
    const QVariantMap math{{QStringLiteral("id"), QStringLiteral("math")}, {QStringLiteral("text"), QStringLiteral("x plus y")},
                           {QStringLiteral("lang"), QStringLiteral("en")}, {QStringLiteral("type"), QStringLiteral("math")}};
    speech.speakSegments({first, second, english, math});
    QTRY_COMPARE(server.messages().size(), 1);
    const auto batch = server.messages().first();
    QCOMPARE(batch.text, QStringLiteral("Erster Satz.Zweiter Satz."));
    QCOMPARE(batch.marks.size(), 4);
    QVERIFY(batch.rawText.contains(QStringLiteral("<s>")));
    QCOMPARE(batch.rawText.count(QStringLiteral("<break time=\"200ms\"/>")), 1);
    const auto pausePosition = batch.rawText.indexOf(QStringLiteral("<break"));
    QVERIFY(pausePosition > batch.rawText.indexOf(QStringLiteral("</s>")));
    QVERIFY(pausePosition < batch.rawText.indexOf(batch.marks.at(2).name));
    QTRY_COMPARE(started.size(), 1);
    // Preparing both sentences must not advance the visible highlight.
    QCOMPARE(started.first().at(1).toMap(), first);
    server.sendIndexMark(batch.id, batch.client, batch.marks.at(1).name);
    QTRY_COMPARE(reached.size(), 1);
    QCOMPARE(reached.last().at(1).toString(), QStringLiteral("first"));
    server.sendIndexMark(batch.id, batch.client, batch.marks.at(2).name);
    QTRY_COMPARE(started.size(), 2);
    QCOMPARE(finished.size(), 1);
    QCOMPARE(finished.first().at(1).toString(), QStringLiteral("first"));
    QCOMPARE(started.last().at(1).toMap(), second);
    // A boundary still underlines the whole sentence without word callbacks.
    QCOMPARE(reached.size(), 1);
    server.sendIndexMark(batch.id, batch.client, batch.marks.at(3).name);
    QTRY_COMPARE(reached.size(), 2);
    QCOMPARE(reached.last().at(1).toString(), QStringLiteral("second"));
    QCOMPARE(reached.last().at(2).toMap(), second.value(QStringLiteral("marks")).toList().first().toMap());
    server.sendIndexMark(batch.id, batch.client, batch.marks.at(1).name);
    QTest::qWait(100);
    QCOMPARE(reached.size(), 2); // A late word must not return to the first sentence.
    speech.pause();
    QTRY_COMPARE(speech.state(), SpeechController::Paused);
    QVERIFY(server.commands().contains(QStringLiteral("CANCEL SELF")));
    speech.resume();
    QTRY_COMPARE(server.messages().size(), 2);
    // Resume repeats only the current sentence, never the completed first one.
    const auto resumed = server.messages().last();
    QCOMPARE(resumed.text, QStringLiteral("Zweiter Satz."));
    QVERIFY(!resumed.rawText.contains(QStringLiteral("<break")));
    server.sendIndexMark(batch.id, batch.client, batch.marks.first().name);
    server.sendOldEnd(batch.id, batch.client);
    QTest::qWait(100);
    QCOMPARE(speech.state(), SpeechController::Speaking);
    QCOMPARE(reached.size(), 2);
    server.finish(resumed.id);
    QTRY_COMPARE(server.messages().size(), 3);
    QCOMPARE(server.messages().last().language, QStringLiteral("en"));
    QCOMPARE(server.messages().last().text, QStringLiteral("Third sentence."));
    server.finish(server.messages().last().id);
    QTRY_COMPARE(server.messages().size(), 4);
    QCOMPARE(server.messages().last().text, QStringLiteral("x plus y"));
    QCOMPARE(server.messages().last().dataMode, QStringLiteral("TEXT"));
    server.finish(server.messages().last().id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    // Rejected SSML restores all sentences to the original queue.
    server.rejectSsml(true);
    speech.speakSegments({first, second});
    QTRY_COMPARE(server.messages().size(), 5);
    QCOMPARE(server.messages().last().text, QStringLiteral("Erster Satz."));
    server.finish(server.messages().last().id);
    QTRY_COMPARE(server.messages().size(), 6);
    QCOMPARE(server.messages().last().text, QStringLiteral("Zweiter Satz."));
    server.finish(server.messages().last().id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
}

void SpeechControllerTest::sentencePause_data()
{
    QTest::addColumn<int>("basePause");
    QTest::addColumn<int>("rate");
    QTest::addColumn<int>("expectedPause");
    QTest::newRow("default") << 200 << 0 << 200;
    QTest::newRow("faster") << 200 << 50 << 133;
    QTest::newRow("fastest") << 200 << 100 << 67;
    QTest::newRow("slower") << 200 << -50 << 400;
    QTest::newRow("slowest") << 200 << -100 << 600;
    QTest::newRow("custom-faster") << 125 << 30 << 100;
    QTest::newRow("custom-slower") << 125 << -30 << 200;
    QTest::newRow("disabled") << 0 << 0 << 0;
    QTest::newRow("disabled-slow") << 0 << -100 << 0;
    QTest::newRow("maximum-base") << 500 << -100 << 1500;
    QTest::newRow("clamp-base-high") << 600 << 0 << 500;
    QTest::newRow("clamp-base-low") << -1 << 0 << 0;
}

void SpeechControllerTest::sentencePause()
{
    QFETCH(int, basePause);
    QFETCH(int, rate);
    QFETCH(int, expectedPause);
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), false, QStringLiteral("piper"));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    Config::setSpeechSentencePauseMs(basePause);
    SpeechController speech;
    speech.adjustSpeechRate(rate);
    const QVariantMap first{{QStringLiteral("text"), QStringLiteral("Erster Satz.")}, {QStringLiteral("lang"), QStringLiteral("de")},
        {QStringLiteral("blockId"), QStringLiteral("paragraph")}, {QStringLiteral("sentenceId"), QStringLiteral("first")}};
    auto second = first;
    second.insert(QStringLiteral("text"), QStringLiteral("Zweiter Satz."));
    second.insert(QStringLiteral("sentenceId"), QStringLiteral("second"));
    speech.speakSegments({first, second});
    QTRY_COMPARE(server.messages().size(), 1);
    const auto message = server.messages().first();
    QCOMPARE(message.text, QStringLiteral("Erster Satz.Zweiter Satz."));
    QCOMPARE(message.rawText.count(QStringLiteral("<s>")), 2);
    QCOMPARE(message.marks.size(), 2);
    if (expectedPause) {
        const QString pause = QStringLiteral("<break time=\"%1ms\"/>").arg(expectedPause);
        QCOMPARE(message.rawText.count(pause), 1);
        QVERIFY(message.rawText.indexOf(pause) < message.rawText.indexOf(message.marks.last().name));
    } else {
        QVERIFY(!message.rawText.contains(QStringLiteral("<break")));
    }
    server.finish(message.id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    // A lone sentence must not receive an initial or final pause.
    speech.speakSegments({first});
    QTRY_COMPARE(server.messages().size(), 2);
    QVERIFY(!server.messages().last().rawText.contains(QStringLiteral("<break")));
}

void SpeechControllerTest::sentencePauseChanges()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), false, QStringLiteral("piper"));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QVariantList segments;
    for (int i = 0; i < 6; ++i) {
        segments.append(QVariantMap{{QStringLiteral("text"), QStringLiteral("Satz Nummer %1.").arg(i)},
            {QStringLiteral("lang"), QStringLiteral("de")}, {QStringLiteral("blockId"), QString::number(i / 2)},
            {QStringLiteral("sentenceId"), QString::number(i)}});
    }
    speech.speakSegments(segments);
    QTRY_COMPARE(server.messages().size(), 1);
    const auto original = server.messages().first();
    QVERIFY(original.rawText.contains(QStringLiteral("<break time=\"200ms\"/>")));
    Config::setSpeechSentencePauseMs(300);
    speech.adjustSpeechRate(30);
    // Settings do not cancel already submitted audio.
    QCOMPARE(server.messages().size(), 1);
    QVERIFY(!server.commands().contains(QStringLiteral("CANCEL SELF")));
    server.finish(original.id);
    QTRY_COMPARE(server.messages().size(), 2);
    QVERIFY(server.messages().last().rawText.contains(QStringLiteral("<break time=\"240ms\"/>")));
    speech.pause();
    QTRY_COMPARE(speech.state(), SpeechController::Paused);
    Config::setSpeechSentencePauseMs(0);
    speech.resume();
    QTRY_COMPARE(server.messages().size(), 3);
    QVERIFY(!server.messages().last().rawText.contains(QStringLiteral("<break")));
    Config::setSpeechSentencePauseMs(500);
    speech.adjustSpeechRate(-80); // +30 -> -50
    server.finish(server.messages().last().id);
    QTRY_COMPARE(server.messages().size(), 4);
    QVERIFY(server.messages().last().rawText.contains(QStringLiteral("<break time=\"1000ms\"/>")));
    server.finish(server.messages().last().id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
}

void SpeechControllerTest::paragraphPause_data()
{
    QTest::addColumn<int>("sentenceBase");
    QTest::addColumn<int>("paragraphBase");
    QTest::addColumn<int>("rate");
    QTest::addColumn<int>("expectedSentence");
    QTest::addColumn<int>("expectedParagraph");
    QTest::newRow("default") << 200 << 400 << 0 << 200 << 400;
    QTest::newRow("faster") << 200 << 400 << 50 << 133 << 267;
    QTest::newRow("fastest") << 200 << 400 << 100 << 67 << 133;
    QTest::newRow("slower") << 200 << 400 << -50 << 400 << 800;
    QTest::newRow("slowest") << 200 << 400 << -100 << 600 << 1200;
    QTest::newRow("custom") << 100 << 300 << 30 << 80 << 240;
    QTest::newRow("paragraph-disabled") << 200 << 0 << 0 << 200 << 0;
    QTest::newRow("sentence-disabled") << 0 << 400 << 0 << 0 << 400;
    QTest::newRow("both-disabled") << 0 << 0 << 0 << 0 << 0;
    QTest::newRow("maximum-base") << 200 << 500 << -100 << 600 << 1500;
    QTest::newRow("clamp-base-high") << 200 << 600 << 0 << 200 << 500;
    QTest::newRow("clamp-base-low") << 200 << -1 << 0 << 200 << 0;
}

void SpeechControllerTest::paragraphPause()
{
    QFETCH(int, sentenceBase);
    QFETCH(int, paragraphBase);
    QFETCH(int, rate);
    QFETCH(int, expectedSentence);
    QFETCH(int, expectedParagraph);
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), false, QStringLiteral("piper"));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    Config::setSpeechSentencePauseMs(sentenceBase);
    Config::setSpeechParagraphPauseMs(paragraphBase);
    SpeechController speech;
    speech.adjustSpeechRate(rate);
    QVariantList segments;
    for (int i = 0; i < 4; ++i) {
        segments.append(QVariantMap{{QStringLiteral("text"), QStringLiteral("Satz %1.").arg(i)},
            {QStringLiteral("lang"), QStringLiteral("de")}, {QStringLiteral("blockId"), QString::number(i / 2)},
            {QStringLiteral("blockTag"), QStringLiteral("p")}, {QStringLiteral("sentenceId"), QString::number(i)}});
    }
    speech.speakSegments(segments);
    QTRY_COMPARE(server.messages().size(), 1);
    const auto message = server.messages().first();
    QCOMPARE(packageTexts(message.rawText).size(), 2);
    QCOMPARE(message.rawText.count(QStringLiteral("<s>")), 4);
    QStringList pauses;
    QXmlStreamReader xml(message.rawText);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("break"))
            pauses.append(xml.attributes().value(QStringLiteral("time")).toString());
    }
    QVERIFY(!xml.hasError());
    QStringList expected;
    for (const int pause : {expectedSentence, expectedParagraph, expectedSentence})
        if (pause) expected.append(QString::number(pause) + QStringLiteral("ms"));
    QCOMPARE(pauses, expected);
    // There is one boundary mark per sentence, and the paragraph mark is after
    // its pause so DOM highlighting continues to follow actual playback.
    QCOMPARE(message.marks.size(), 4);
    if (expectedParagraph) {
        const auto boundary = message.rawText.indexOf(message.marks.at(2).name);
        const auto pause = message.rawText.lastIndexOf(QStringLiteral("<break"), boundary);
        QVERIFY(pause > message.rawText.indexOf(message.marks.at(1).name));
        QVERIFY(pause < boundary);
    }
    server.finish(message.id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    speech.speakSegments({segments.first()});
    QTRY_COMPARE(server.messages().size(), 2);
    QVERIFY(!server.messages().last().rawText.contains(QStringLiteral("<break")));
}

void SpeechControllerTest::paragraphPauseChanges()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), false, QStringLiteral("piper"));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QVariantList segments;
    for (int i = 0; i < 20; ++i) {
        segments.append(QVariantMap{{QStringLiteral("text"), QStringLiteral("Satz %1.").arg(i)},
            {QStringLiteral("lang"), QStringLiteral("de")}, {QStringLiteral("blockId"), QString::number(i / 2)},
            {QStringLiteral("blockTag"), QStringLiteral("p")}, {QStringLiteral("sentenceId"), QString::number(i)}});
    }
    speech.speakSegments(segments);
    QTRY_COMPARE(server.messages().size(), 1);
    const auto original = server.messages().first();
    QCOMPARE(original.rawText.count(QStringLiteral("<break time=\"400ms\"/>")), 3);
    Config::setSpeechParagraphPauseMs(300);
    speech.adjustSpeechRate(30);
    QVERIFY(!server.commands().contains(QStringLiteral("CANCEL SELF")));
    QCOMPARE(server.messages().size(), 1);
    server.finish(original.id);
    QTRY_COMPARE(server.messages().size(), 2);
    const auto changed = server.messages().last();
    QCOMPARE(changed.rawText.count(QStringLiteral("<break time=\"240ms\"/>")), 3);
    QCOMPARE(changed.rawText.count(QStringLiteral("<break time=\"160ms\"/>")), 4);
    speech.pause();
    QTRY_COMPARE(speech.state(), SpeechController::Paused);
    Config::setSpeechParagraphPauseMs(0);
    speech.resume();
    QTRY_COMPARE(server.messages().size(), 3);
    QVERIFY(!server.messages().last().rawText.contains(QStringLiteral("<break time=\"240ms\"/>")));
    QCOMPARE(server.messages().last().rawText.count(QStringLiteral("<break time=\"160ms\"/>")), 4);
    Config::setSpeechParagraphPauseMs(500);
    speech.adjustSpeechRate(-80);
    server.finish(server.messages().last().id);
    QTRY_COMPARE(server.messages().size(), 4);
    QCOMPARE(server.messages().last().rawText.count(QStringLiteral("<break time=\"1000ms\"/>")), 1);
    QCOMPARE(server.messages().last().rawText.count(QStringLiteral("<break time=\"400ms\"/>")), 2);
    server.finish(server.messages().last().id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
}

void SpeechControllerTest::sentencePauseSettings()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import org.kde.kirigami as Kirigami
        import org.kde.arianna
        Kirigami.ApplicationWindow {
            width: 900; height: 900; visible: true
            pageStack.initialPage: SettingsPage {}
        }
    )", QUrl());
    QTRY_VERIFY(component.status() != QQmlComponent::Loading);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *field = object->findChild<QObject *>(QStringLiteral("speechSentencePauseSetting"));
    auto *numbers = object->findChild<QQuickItem *>(QStringLiteral("speechSanskritNumbersSwitch"));
    QVERIFY(numbers && numbers->isVisible());
    QVERIFY(!Config::defaultSpeechExpandSanskritNumbersValue());
    QVERIFY(!numbers->property("checked").toBool());
    QCOMPARE(numbers->property("text").toString(), i18nc("@option:check", "Spell out Sanskrit numbers when reading aloud"));
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    QVERIFY(window && QTest::qWaitForWindowExposed(window));
    numbers->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_VERIFY(Config::speechExpandSanskritNumbers());
    QVERIFY(field);
    QCOMPARE(field->property("from").toInt(), 0);
    QCOMPARE(field->property("to").toInt(), 500);
    QCOMPARE(field->property("stepSize").toInt(), 1);
    QCOMPARE(field->property("value").toInt(), 200);
    QVERIFY(QMetaObject::invokeMethod(field, "increase"));
    QCOMPARE(Config::speechSentencePauseMs(), 201);
    QVERIFY(field->setProperty("value", -1));
    QCOMPARE(Config::speechSentencePauseMs(), 0);
    QVERIFY(QMetaObject::invokeMethod(field, "decrease"));
    QCOMPARE(Config::speechSentencePauseMs(), 0);
    QVERIFY(field->setProperty("value", 501));
    QCOMPARE(Config::speechSentencePauseMs(), 500);
    QVERIFY(QMetaObject::invokeMethod(field, "increase"));
    QCOMPARE(Config::speechSentencePauseMs(), 500);
    auto *paragraph = object->findChild<QObject *>(QStringLiteral("speechParagraphPauseSetting"));
    QVERIFY(paragraph);
    QCOMPARE(Config::defaultSpeechParagraphPauseMsValue(), 400);
    QCOMPARE(paragraph->property("from").toInt(), 0);
    QCOMPARE(paragraph->property("to").toInt(), 500);
    QCOMPARE(paragraph->property("stepSize").toInt(), 1);
    QCOMPARE(paragraph->property("value").toInt(), 400);
    QCOMPARE(paragraph->property("label").toString(), i18nc("@label:spinbox", "Paragraph pause length in ms"));
    QVERIFY(QMetaObject::invokeMethod(paragraph, "increase"));
    QCOMPARE(Config::speechParagraphPauseMs(), 401);
    QVERIFY(paragraph->setProperty("value", -1));
    QCOMPARE(Config::speechParagraphPauseMs(), 0);
    QVERIFY(paragraph->setProperty("value", 501));
    QCOMPARE(Config::speechParagraphPauseMs(), 500);
    QVERIFY(paragraph->setProperty("value", 450));
    QCOMPARE(Config::speechParagraphPauseMs(), 450);
    QCOMPARE(Config::speechSentencePauseMs(), 500);
    KConfig saved(Config::self()->config()->name(), KConfig::SimpleConfig);
    QCOMPARE(KConfigGroup(&saved, QStringLiteral("Speech")).readEntry(QStringLiteral("speechSentencePauseMs"), -1), 500);
    QCOMPARE(KConfigGroup(&saved, QStringLiteral("Speech")).readEntry(QStringLiteral("speechParagraphPauseMs"), -1), 450);
    QVERIFY(KConfigGroup(&saved, QStringLiteral("Speech")).readEntry(QStringLiteral("speechExpandSanskritNumbers"), false));
    object.reset();
    Config::setSpeechSentencePauseMs(100);
    Config::setSpeechParagraphPauseMs(100);
    Config::setSpeechExpandSanskritNumbers(false);
    Config::self()->load();
    QCOMPARE(Config::speechSentencePauseMs(), 500);
    QCOMPARE(Config::speechParagraphPauseMs(), 450);
    QVERIFY(Config::speechExpandSanskritNumbers());
    object.reset(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    numbers = object->findChild<QQuickItem *>(QStringLiteral("speechSanskritNumbersSwitch"));
    QVERIFY(numbers && numbers->property("checked").toBool());
    window = qobject_cast<QQuickWindow *>(object.get());
    QVERIFY(window && QTest::qWaitForWindowExposed(window));
    numbers->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_VERIFY(!Config::speechExpandSanskritNumbers());
    saved.reparseConfiguration();
    // KConfig removes keys whose saved value is the schema default (false).
    QVERIFY(!KConfigGroup(&saved, QStringLiteral("Speech")).readEntry(QStringLiteral("speechExpandSanskritNumbers"), false));
    object.reset();
    Config::setSpeechExpandSanskritNumbers(true);
    Config::self()->load();
    QVERIFY(!Config::speechExpandSanskritNumbers());
}

void SpeechControllerTest::preparedInlineMath()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), false, QStringLiteral("piper"));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    QSignalSpy reached(&speech, &SpeechController::indexMarkReached);
    const auto part = [](const QString &id, const QString &text, const QString &sentence, bool math = false) {
        QVariantMap segment{{QStringLiteral("id"), id}, {QStringLiteral("text"), text}, {QStringLiteral("lang"), QStringLiteral("de")},
            {QStringLiteral("blockId"), QStringLiteral("math-paragraph")}, {QStringLiteral("sentenceId"), sentence},
            {QStringLiteral("href"), QStringLiteral("chapter.xhtml")}};
        if (math) {
            segment.insert(QStringLiteral("type"), QStringLiteral("math"));
            segment.insert(QStringLiteral("inline"), true);
        } else {
            segment.insert(QStringLiteral("marks"), QVariantList{QVariantMap{
                {QStringLiteral("start"), 0}, {QStringLiteral("length"), text.indexOf(QLatin1Char(' '))},
                {QStringLiteral("cfi"), QString(id + QStringLiteral("-word"))}}});
        }
        return segment;
    };
    const auto before = part(QStringLiteral("before"), QStringLiteral("Vor der Formel"), QStringLiteral("one"));
    const auto formula = part(QStringLiteral("formula"), QStringLiteral("x plus eins"), QStringLiteral("one"), true);
    const auto after = part(QStringLiteral("after"), QStringLiteral("folgt Text."), QStringLiteral("one"));
    const auto next = part(QStringLiteral("next"), QStringLiteral("Nächster Satz."), QStringLiteral("two"));
    auto display = formula;
    display.insert(QStringLiteral("id"), QStringLiteral("display"));
    display.insert(QStringLiteral("inline"), false);
    speech.speakSegments({before, formula, after, next, display});
    QTRY_COMPARE(server.messages().size(), 1);
    const auto message = server.messages().first();
    QCOMPARE(message.text, QStringLiteral("Vor der Formel x plus eins folgt Text.Nächster Satz."));
    QCOMPARE(message.rawText.count(QStringLiteral("<s>")), 2);
    QCOMPARE(message.rawText.count(QStringLiteral("<break time=\"200ms\"/>")), 1);
    QCOMPARE(message.marks.size(), 7); // Four segments, three prose word marks.
    for (const auto &mark : message.marks) server.sendIndexMark(message.id, message.client, mark.name);
    QTRY_COMPARE(started.size(), 4);
    QTRY_COMPARE(reached.size(), 3);
    QCOMPARE(started.at(1).at(1).toMap(), formula);
    QCOMPARE(reached.at(1).at(1).toString(), QStringLiteral("after"));
    QCOMPARE(reached.at(1).at(2).toMap().value(QStringLiteral("start")).toInt(), 0);
    server.finish(message.id);
    QTRY_COMPARE(server.messages().size(), 2);
    QCOMPARE(server.messages().last().text, QStringLiteral("x plus eins"));
    QCOMPARE(server.messages().last().dataMode, QStringLiteral("TEXT"));
    server.finish(server.messages().last().id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    // Resuming during the formula repeats the formula and following text only.
    speech.speakSegments({before, formula, after});
    QTRY_COMPARE(server.messages().size(), 3);
    const auto paused = server.messages().last();
    server.sendIndexMark(paused.id, paused.client, paused.marks.at(2).name);
    QTRY_COMPARE(started.last().at(1).toMap(), formula);
    speech.pause();
    QTRY_COMPARE(speech.state(), SpeechController::Paused);
    speech.resume();
    QTRY_COMPARE(server.messages().size(), 4);
    const auto resumed = server.messages().last();
    QCOMPARE(resumed.text, QStringLiteral("x plus eins folgt Text."));
    QCOMPARE(resumed.rawText.count(QStringLiteral("<s>")), 1);
    QVERIFY(!resumed.rawText.contains(QStringLiteral("<break")));
    server.finish(resumed.id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    auto punctuation = after;
    punctuation.insert(QStringLiteral("text"), QStringLiteral("."));
    punctuation.insert(QStringLiteral("marks"), QVariantList{});
    speech.speakSegments({before, formula, punctuation, next});
    QTRY_COMPARE(server.messages().size(), 5);
    const auto punctuated = server.messages().last();
    QCOMPARE(punctuated.text, QStringLiteral("Vor der Formel x plus eins.Nächster Satz."));
    QCOMPARE(punctuated.rawText.count(QStringLiteral("<s>")), 2);
    QCOMPARE(punctuated.rawText.count(QStringLiteral("<break time=\"200ms\"/>")), 1);
    server.finish(punctuated.id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
}

void SpeechControllerTest::paragraphPackages()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), false, QStringLiteral("piper"));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    const auto segment = [](const QString &text, const QString &blockId, const QString &tag = QStringLiteral("p")) {
        return QVariantMap{{QStringLiteral("text"), text}, {QStringLiteral("lang"), QStringLiteral("de")},
            {QStringLiteral("href"), QStringLiteral("chapter.xhtml")},
            {QStringLiteral("blockId"), blockId}, {QStringLiteral("blockTag"), tag},
            {QStringLiteral("marks"), QVariantList{QVariantMap{{QStringLiteral("start"), 0},
                {QStringLiteral("length"), text.indexOf(QLatin1Char(' '))}}}}};
    };
    QVariantList segments;
    QString paragraph;
    for (int i = 1; i <= 6; ++i) {
        const QString text = QStringLiteral("Satz Nummer %1. ").arg(i);
        segments.append(segment(text, QStringLiteral("paragraph-one")));
        paragraph += text;
    }
    segments.append(segment(QStringLiteral("Eine Überschrift. "), QStringLiteral("heading-one"), QStringLiteral("h2")));
    segments.append(segment(QStringLiteral("Mit Untertitel."), QStringLiteral("heading-one"), QStringLiteral("h2")));
    segments.append(segment(QStringLiteral("Andere Überschrift"), QStringLiteral("heading-two"), QStringLiteral("h2")));
    segments.append(segment(QStringLiteral("Ein neuer Absatz. "), QStringLiteral("paragraph-two")));
    segments.append(segment(QStringLiteral("Zweiter Satz."), QStringLiteral("paragraph-two")));
    segments.append(segment(QStringLiteral("Unbekannter Block eins."), QString()));
    segments.append(segment(QStringLiteral("Unbekannter Block zwei."), QString()));
    speech.speakSegments(segments);
    const QStringList expected{paragraph, QStringLiteral("Eine Überschrift. Mit Untertitel."),
        QStringLiteral("Andere Überschrift"), QStringLiteral("Ein neuer Absatz. Zweiter Satz."),
        QStringLiteral("Unbekannter Block eins."), QStringLiteral("Unbekannter Block zwei.")};
    for (qsizetype i = 0; i < expected.size(); ++i) {
        QTRY_COMPARE(server.messages().size(), i + 1);
        const auto message = server.messages().last();
        QCOMPARE(message.text, expected.at(i));
        if (i == 0) {
            QCOMPARE(message.rawText.count(QStringLiteral("<s>")), 6);
            QCOMPARE(message.rawText.count(QStringLiteral("<break time=\"200ms\"/>")), 5);
        }
        server.finish(message.id);
    }
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    // Long paragraphs remain bounded, without dropping/reordering sentences.
    segments.clear();
    const QString text = QStringLiteral("Ein langer Satz ") + QString(180, QLatin1Char('a')) + QStringLiteral(". ");
    for (int i = 0; i < 11; ++i) segments.append(segment(text, QStringLiteral("long-paragraph")));
    speech.speakSegments(segments);
    QString spoken;
    qsizetype messages = expected.size();
    const qsizetype longParagraphStart = messages;
    while (spoken.size() < text.size() * 11) {
        ++messages;
        QTRY_COMPARE(server.messages().size(), messages);
        const auto message = server.messages().last();
        QVERIFY(message.text.size() <= 8192);
        for (const auto &package : packageTexts(message.rawText)) QVERIFY(package.size() <= 2048);
        if (spoken.isEmpty()) {
            QCOMPARE(message.text, text.repeated(11));
            QCOMPARE(packageTexts(message.rawText), QStringList({text.repeated(10), text}));
        }
        spoken += message.text;
        server.finish(message.id);
    }
    QCOMPARE(spoken, text.repeated(11));
    QCOMPARE(messages - longParagraphStart, 1);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    // Pronunciation aliases can be longer than the source EPUB text.
    segments.clear();
    const QString iast = QStringLiteral("Śrī Kṛṣṇa ").repeated(20) + QStringLiteral(". ");
    const QString alias = QStringLiteral("Schri Krischna ").repeated(20) + QStringLiteral(". ");
    for (int i = 0; i < 8; ++i) segments.append(segment(iast, QStringLiteral("iast-paragraph")));
    speech.speakSegments(segments);
    spoken.clear();
    const qsizetype iastParagraphStart = messages;
    while (spoken.size() < alias.size() * 8) {
        ++messages;
        QTRY_COMPARE(server.messages().size(), messages);
        const auto message = server.messages().last();
        QVERIFY(message.text.size() <= 8192);
        for (const auto &package : packageTexts(message.rawText)) QVERIFY(package.size() <= 2048);
        if (spoken.isEmpty()) {
            QCOMPARE(message.text, alias.repeated(8));
            QCOMPARE(packageTexts(message.rawText), QStringList({alias.repeated(6), alias.repeated(2)}));
        }
        spoken += message.text;
        server.finish(message.id);
    }
    QCOMPARE(spoken, alias.repeated(8));
    QCOMPARE(messages - iastParagraphStart, 1);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
}

void SpeechControllerTest::preloadedParagraphs()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), false, QStringLiteral("piper"));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    QSignalSpy reached(&speech, &SpeechController::indexMarkReached);
    QVariantList paragraphs;
    for (int i = 0; i < 5; ++i) {
        paragraphs.append(QVariantMap{{QStringLiteral("id"), QString::number(i)},
            {QStringLiteral("text"), QStringLiteral("Absatz %1. ").arg(i)}, {QStringLiteral("lang"), QStringLiteral("de")},
            {QStringLiteral("href"), QStringLiteral("chapter.xhtml")}, {QStringLiteral("blockId"), QString::number(i)},
            {QStringLiteral("blockTag"), QStringLiteral("p")}, {QStringLiteral("sentenceId"), QStringLiteral("one")},
            {QStringLiteral("marks"), QVariantList{QVariantMap{{QStringLiteral("start"), 0}, {QStringLiteral("length"), 6},
                {QStringLiteral("cfi"), QStringLiteral("word-%1").arg(i)}}}}});
    }
    speech.speakSegments(paragraphs);
    QTRY_COMPARE(server.messages().size(), 1);
    const auto original = server.messages().first();
    QCOMPARE(packageTexts(original.rawText).size(), 4);
    QCOMPARE(original.rawText.count(QStringLiteral("<s>")), 4);
    QCOMPARE(original.rawText.count(QStringLiteral("<break time=\"400ms\"/>")), 3);
    QVERIFY(!original.text.contains(QStringLiteral("Absatz 4")));
    QTRY_COMPARE(started.size(), 1); // Preparing paragraphs cannot advance the highlight.
    server.sendIndexMark(original.id, original.client, original.marks.at(2).name);
    server.sendIndexMark(original.id, original.client, original.marks.at(3).name);
    QTRY_COMPARE(started.size(), 2);
    QTRY_COMPARE(reached.size(), 1);
    QCOMPARE(started.last().at(1).toMap(), paragraphs.at(1).toMap());
    QCOMPARE(reached.last().at(2).toMap(), paragraphs.at(1).toMap().value(QStringLiteral("marks")).toList().first().toMap());
    speech.pause();
    QTRY_COMPARE(speech.state(), SpeechController::Paused);
    speech.adjustSpeechRate(50);
    speech.resume();
    QTRY_COMPARE(server.messages().size(), 2);
    const auto resumed = server.messages().last();
    QCOMPARE(packageTexts(resumed.rawText).size(), 4);
    QVERIFY(!resumed.text.contains(QStringLiteral("Absatz 0")));
    QVERIFY(resumed.text.endsWith(QStringLiteral("Absatz 4. ")));
    QCOMPARE(resumed.rawText.count(QStringLiteral("<break time=\"267ms\"/>")), 3);
    server.sendOldEnd(original.id, original.client);
    server.sendIndexMark(original.id, original.client, original.marks.first().name);
    QTest::qWait(50);
    QCOMPARE(server.messages().size(), 2);
    QCOMPARE(reached.size(), 1);
    speech.stop();
    server.sendOldEnd(resumed.id, resumed.client);
    QTest::qWait(50);
    QCOMPARE(speech.state(), SpeechController::Idle);
    QCOMPARE(server.messages().size(), 2);
    // SSML rejection must retain every paragraph in the plain-text fallback.
    server.rejectSsml(true);
    speech.speakSegments(paragraphs);
    for (int i = 0; i < paragraphs.size(); ++i) {
        QTRY_COMPARE(server.messages().size(), i + 3);
        const auto message = server.messages().last();
        QCOMPARE(message.text, paragraphs.at(i).toMap().value(QStringLiteral("text")).toString());
        QCOMPARE(message.dataMode, QStringLiteral("TEXT"));
        server.finish(message.id);
    }
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    QVERIFY(speech.errorString().isEmpty());
}

void SpeechControllerTest::continuousPreload()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), false, QStringLiteral("piper"));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QSignalSpy refill(&speech, &SpeechController::moreSegmentsRequested);
    const QVariantList first{QVariantMap{{QStringLiteral("text"), QStringLiteral("First paragraph.")}, {QStringLiteral("lang"), QStringLiteral("en")}}};
    const QVariantList second{QVariantMap{{QStringLiteral("text"), QStringLiteral("Zweiter Absatz.")}, {QStringLiteral("lang"), QStringLiteral("de")}}};
    speech.startReading(first, true);
    const QString session = speech.sessionId();
    QTRY_COMPARE(server.messages().size(), 1);
    QTRY_COMPARE(refill.size(), 1); // Request text before the playing message ends.
    QCOMPARE(refill.first().first().toString(), session);
    QVERIFY(refill.first().at(1).toBool());
    speech.pause();
    QTRY_COMPARE(speech.state(), SpeechController::Paused);
    speech.continueReading(session, second, true);
    server.finish(server.messages().last().id);
    QTest::qWait(50);
    QCOMPARE(server.messages().size(), 1);
    speech.adjustSpeechRate(20);
    speech.resume();
    QTRY_COMPARE(server.messages().size(), 2);
    QTRY_COMPARE(refill.size(), 2);
    QCOMPARE(server.messages().last().text, QStringLiteral("Zweiter Absatz."));
    QVERIFY(server.commands().contains(QStringLiteral("SET SELF RATE 20")));
    QVERIFY(refill.last().at(1).toBool());
    // The reader cannot preload a chapter that requires visible navigation.
    speech.continueReading(session, {}, true);
    QTest::qWait(50);
    QCOMPARE(refill.size(), 2);
    server.finish(server.messages().last().id);
    QTRY_COMPARE(refill.size(), 3);
    QVERIFY(!refill.last().at(1).toBool());
    speech.continueReading(session, first, false);
    QTRY_COMPARE(server.messages().size(), 3);
    server.finish(server.messages().last().id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    speech.startReading(first, true);
    QTRY_COMPARE(server.messages().size(), 4);
    QTRY_COMPARE(refill.size(), 4);
    const auto stopped = server.messages().last();
    speech.stop();
    speech.continueReading(session, second, false);
    speech.continueReading(speech.sessionId(), second, false);
    server.sendOldEnd(stopped.id, stopped.client);
    QTest::qWait(50);
    QCOMPARE(server.messages().size(), 4);
    QCOMPARE(speech.state(), SpeechController::Idle);
}

void SpeechControllerTest::realIndexMarks_data()
{
    QTest::addColumn<QString>("language");
    QTest::addColumn<QString>("text");
    QTest::newRow("de") << QStringLiteral("de") << QStringLiteral("Diese Wörter werden deutlich vorgelesen.");
    QTest::newRow("en") << QStringLiteral("en") << QStringLiteral("Hello world this is clear speech.");
    QTest::newRow("de-iast") << QStringLiteral("de") << QStringLiteral("Wir lesen Śaṅkarācārya Bhagavad-gītā Śrī Kṛṣṇa.");
    QTest::newRow("en-iast") << QStringLiteral("en") << QStringLiteral("Read Śaṅkarācārya Bhagavad-gītā Śrī Kṛṣṇa clearly.");
    QTest::newRow("de-reference") << QStringLiteral("de") << QStringLiteral("भगवद्गीता, Kapitel 2.");
    QTest::newRow("en-reference") << QStringLiteral("en") << QStringLiteral("भगवद्गीता, Chapter 2.");
}

void SpeechControllerTest::realIndexMarks()
{
    const QString module = qEnvironmentVariable("ARIANNA_TEST_SPEECH_MODULE");
    if (module.isEmpty()) QSKIP("Set ARIANNA_TEST_SPEECH_MODULE to test a real output module.");
    QFETCH(QString, language);
    QFETCH(QString, text);
    const bool reference = QByteArray(QTest::currentDataTag()).endsWith("-reference");
    if (realSpeechAddress.isEmpty()) qunsetenv("SPEECHD_ADDRESS");
    else qputenv("SPEECHD_ADDRESS", realSpeechAddress);
    SpeechController speech;
    speech.refreshVoices();
    QTRY_VERIFY_WITH_TIMEOUT(!speech.voices().isEmpty() || !speech.errorString().isEmpty(), 15000);
    QVERIFY2(speech.errorString().isEmpty(), qPrintable(speech.errorString()));
    QVariantMap chosen;
    for (const auto &value : speech.voices()) {
        const auto voice = value.toMap();
        if (voice.value(QStringLiteral("module")).toString() == module
            && voice.value(QStringLiteral("language")).toString().section(QLatin1Char('-'), 0, 0) == language) {
            chosen = voice;
            break;
        }
    }
    QVERIFY2(!chosen.isEmpty(), qPrintable(QStringLiteral("Voice unavailable: %1 / %2").arg(module, language)));
    Config::setSpeechVoice(chosen.value(QStringLiteral("id")).toString());
    QSignalSpy reached(&speech, &SpeechController::indexMarkReached);
    QVariantList marks;
    qsizetype start = 0;
    for (const auto &word : text.chopped(1).split(QLatin1Char(' '))) {
        QVariantMap mark{{QStringLiteral("start"), start}, {QStringLiteral("length"), word.size()}};
        if (reference) mark.insert(QStringLiteral("cfi"), QStringLiteral("original-anchor"));
        marks.append(mark);
        start += word.size() + 1;
    }
    speech.speakSegments({QVariantMap{
        {QStringLiteral("text"), text}, {QStringLiteral("id"), QStringLiteral("real-test")},
        {QStringLiteral("lang"), language}, {QStringLiteral("marks"), marks},
        {QStringLiteral("type"), reference ? QStringLiteral("reference") : QStringLiteral("text")}}});
    QTRY_VERIFY_WITH_TIMEOUT(reached.size() == marks.size() || !speech.errorString().isEmpty(), 30000);
    QVERIFY2(speech.errorString().isEmpty(), qPrintable(speech.errorString()));
    QCOMPARE(reached.size(), marks.size());
    for (qsizetype i = 0; i < marks.size(); ++i) {
        QCOMPARE(reached.at(i).at(0).toString(), speech.sessionId());
        QCOMPARE(reached.at(i).at(1).toString(), QStringLiteral("real-test"));
        QCOMPARE(reached.at(i).at(2).toMap(), marks.at(i).toMap());
    }
    QTRY_COMPARE_WITH_TIMEOUT(speech.state(), SpeechController::Idle, 15000);
    qInfo().noquote() << "Real index marks received from" << module << chosen.value(QStringLiteral("voice")).toString();
}

void SpeechControllerTest::realSentenceModeChanges_data()
{
    QTest::addColumn<bool>("repeat");
    QTest::newRow("once") << false;
    QTest::newRow("repeat") << true;
}

void SpeechControllerTest::realSentenceModeChanges()
{
    QFETCH(bool, repeat);
    if (qEnvironmentVariable("ARIANNA_TEST_SPEECH_MODULE") != QLatin1String("piper"))
        QSKIP("Set ARIANNA_TEST_SPEECH_MODULE=piper to test sentence mode changes during real playback.");
    if (realSpeechAddress.isEmpty()) qunsetenv("SPEECHD_ADDRESS");
    else qputenv("SPEECHD_ADDRESS", realSpeechAddress);
    SpeechController speech;
    speech.refreshVoices();
    QTRY_VERIFY_WITH_TIMEOUT(!speech.voices().isEmpty() || !speech.errorString().isEmpty(), 15000);
    QVERIFY2(speech.errorString().isEmpty(), qPrintable(speech.errorString()));
    const auto voices = speech.voices();
    const auto voice = std::find_if(voices.cbegin(), voices.cend(), [](const auto &value) {
        const auto map = value.toMap();
        return map.value(QStringLiteral("module")).toString() == QLatin1String("piper")
            && map.value(QStringLiteral("language")).toString() == QLatin1String("de");
    });
    QVERIFY(voice != voices.cend());
    Config::setSpeechVoice(voice->toMap().value(QStringLiteral("id")).toString());
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    int currentStarts = 0;
    connect(&speech, &SpeechController::segmentStarted, &speech, [&](const QString &, const QVariantMap &segment) {
        if (segment.value(QStringLiteral("id")).toString() != QLatin1String("current")) return;
        if (++currentStarts == 1) speech.finishSentence(repeat);
        else speech.setRepeatSelection(false);
    });
    const auto segment = [](const QString &id, const QString &text) {
        return QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("text"), text}, {QStringLiteral("lang"), QStringLiteral("de")},
            {QStringLiteral("sentenceId"), id}, {QStringLiteral("blockId"), QStringLiteral("paragraph")},
            {QStringLiteral("blockTag"), QStringLiteral("p")}};
    };
    speech.startReading({segment(QStringLiteral("first"), QStringLiteral("Dies ist der erste Satz.")),
        segment(QStringLiteral("current"), QStringLiteral("Dieser Satz wird jetzt zu Ende gelesen.")),
        segment(QStringLiteral("later"), QStringLiteral("Dieser folgende Satz soll nicht beginnen."))}, true);
    QTRY_VERIFY_WITH_TIMEOUT(speech.state() == SpeechController::Idle || !speech.errorString().isEmpty(), 30000);
    QVERIFY2(speech.errorString().isEmpty(), qPrintable(speech.errorString()));
    QCOMPARE(currentStarts, repeat ? 2 : 1);
    QCOMPARE(started.size(), repeat ? 3 : 2);
    QCOMPARE(started.first().at(1).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("first"));
    for (qsizetype index = 1; index < started.size(); ++index)
        QCOMPARE(started.at(index).at(1).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("current"));
}

void SpeechControllerTest::realPreparedPause_data()
{
    QTest::addColumn<bool>("separateParagraphs");
    QTest::newRow("sentences") << false;
    QTest::newRow("paragraphs") << true;
}

void SpeechControllerTest::realPreparedPause()
{
    QFETCH(bool, separateParagraphs);
    const QString module = qEnvironmentVariable("ARIANNA_TEST_SPEECH_MODULE");
    if (module != QLatin1String("piper")) QSKIP("Set ARIANNA_TEST_SPEECH_MODULE=piper to test buffered pause/resume.");
    if (realSpeechAddress.isEmpty()) qunsetenv("SPEECHD_ADDRESS");
    else qputenv("SPEECHD_ADDRESS", realSpeechAddress);
    SpeechController speech;
    speech.refreshVoices();
    QTRY_VERIFY_WITH_TIMEOUT(!speech.voices().isEmpty() || !speech.errorString().isEmpty(), 15000);
    QVERIFY2(speech.errorString().isEmpty(), qPrintable(speech.errorString()));
    bool found = false;
    for (const auto &value : speech.voices()) {
        const auto voice = value.toMap();
        if (voice.value(QStringLiteral("module")).toString() == module && voice.value(QStringLiteral("language")).toString() == QLatin1String("de")) {
            Config::setSpeechVoice(voice.value(QStringLiteral("id")).toString());
            found = true;
            break;
        }
    }
    QVERIFY(found);
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    QSignalSpy reached(&speech, &SpeechController::indexMarkReached);
    QElapsedTimer elapsed;
    QList<qint64> wordTimes;
    connect(&speech, &SpeechController::indexMarkReached, &speech, [&] {
        wordTimes.append(elapsed.elapsed());
    });
    QVariantList segments;
    for (int i = 0; i < 3; ++i) {
        segments.append(QVariantMap{{QStringLiteral("id"), QString::number(i)}, {QStringLiteral("lang"), QStringLiteral("de")},
            {QStringLiteral("blockId"), separateParagraphs ? QString::number(i) : QStringLiteral("real-paragraph")},
            {QStringLiteral("blockTag"), QStringLiteral("p")},
            {QStringLiteral("text"), QStringLiteral("Diese Wörter werden deutlich vorgelesen.")},
            {QStringLiteral("marks"), QVariantList{QVariantMap{{QStringLiteral("start"), 0}, {QStringLiteral("length"), 5}}}}});
    }
    elapsed.start();
    speech.speakSegments(segments);
    const QString session = speech.sessionId();
    QTRY_VERIFY_WITH_TIMEOUT(started.size() >= 2 || !speech.errorString().isEmpty(), 15000);
    QVERIFY2(speech.errorString().isEmpty(), qPrintable(speech.errorString()));
    QCOMPARE(started.size(), 2);
    QTRY_COMPARE(reached.size(), 2);
    if (separateParagraphs)
        qInfo() << "Preloaded paragraph start interval in ms:" << wordTimes.at(1) - wordTimes.at(0);
    speech.pause();
    QTRY_COMPARE(speech.state(), SpeechController::Paused);
    QTest::qWait(100);
    QCOMPARE(started.size(), 2);
    speech.adjustSpeechRate(20);
    speech.resume();
    QTRY_VERIFY_WITH_TIMEOUT(speech.state() == SpeechController::Idle || !speech.errorString().isEmpty(), 30000);
    QVERIFY2(speech.errorString().isEmpty(), qPrintable(speech.errorString()));
    QCOMPARE(speech.sessionId(), session);
    QCOMPARE(started.size(), 4);
    const QStringList expected{QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("1"), QStringLiteral("2")};
    for (qsizetype i = 0; i < started.size(); ++i) QCOMPARE(started.at(i).at(1).toMap().value(QStringLiteral("id")).toString(), expected.at(i));
}

void SpeechControllerTest::realInlineMath()
{
    if (qEnvironmentVariable("ARIANNA_TEST_SPEECH_MODULE") != QLatin1String("piper"))
        QSKIP("Set ARIANNA_TEST_SPEECH_MODULE=piper to test inline formula index marks.");
    if (realSpeechAddress.isEmpty()) qunsetenv("SPEECHD_ADDRESS");
    else qputenv("SPEECHD_ADDRESS", realSpeechAddress);
    SpeechController speech;
    speech.refreshVoices();
    QTRY_VERIFY_WITH_TIMEOUT(!speech.voices().isEmpty() || !speech.errorString().isEmpty(), 15000);
    QVERIFY2(speech.errorString().isEmpty(), qPrintable(speech.errorString()));
    bool found = false;
    for (const auto &value : speech.voices()) {
        const auto voice = value.toMap();
        if (voice.value(QStringLiteral("module")).toString() == QLatin1String("piper")
            && voice.value(QStringLiteral("language")).toString() == QLatin1String("de")) {
            Config::setSpeechVoice(voice.value(QStringLiteral("id")).toString());
            found = true;
            break;
        }
    }
    QVERIFY(found);
    QSignalSpy started(&speech, &SpeechController::segmentStarted);
    QSignalSpy reached(&speech, &SpeechController::indexMarkReached);
    const QVariantMap common{{QStringLiteral("lang"), QStringLiteral("de")},
        {QStringLiteral("blockId"), QStringLiteral("real-math-paragraph")},
        {QStringLiteral("sentenceId"), QStringLiteral("same-sentence")}};
    auto before = common;
    before.insert(QStringLiteral("id"), QStringLiteral("before"));
    before.insert(QStringLiteral("text"), QStringLiteral("Die Formel lautet "));
    before.insert(QStringLiteral("marks"), QVariantList{QVariantMap{{QStringLiteral("start"), 0}, {QStringLiteral("length"), 3}}});
    auto formula = common;
    formula.insert(QStringLiteral("id"), QStringLiteral("math"));
    formula.insert(QStringLiteral("type"), QStringLiteral("math"));
    formula.insert(QStringLiteral("inline"), true);
    formula.insert(QStringLiteral("text"), QStringLiteral("x plus eins"));
    auto after = common;
    after.insert(QStringLiteral("id"), QStringLiteral("after"));
    after.insert(QStringLiteral("text"), QStringLiteral(" und ist einfach."));
    after.insert(QStringLiteral("marks"), QVariantList{QVariantMap{{QStringLiteral("start"), 1}, {QStringLiteral("length"), 3}}});
    speech.speakSegments({before, formula, after});
    QTRY_VERIFY_WITH_TIMEOUT(!started.isEmpty() || !speech.errorString().isEmpty(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(speech.state() == SpeechController::Idle || !speech.errorString().isEmpty(), 20000);
    QVERIFY2(speech.errorString().isEmpty(), qPrintable(speech.errorString()));
    QCOMPARE(started.size(), 3);
    QCOMPARE(reached.size(), 2);
    QCOMPARE(started.at(0).at(1).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("before"));
    QCOMPARE(started.at(1).at(1).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("math"));
    QCOMPARE(started.at(2).at(1).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("after"));
    QCOMPARE(reached.at(0).at(1).toString(), QStringLiteral("before"));
    QCOMPARE(reached.at(1).at(1).toString(), QStringLiteral("after"));
    QCOMPARE(reached.at(1).at(2).toMap().value(QStringLiteral("start")).toInt(), 1);
}

void SpeechControllerTest::realViewerIndexMarks_data()
{
    QTest::addColumn<bool>("continuous");
    QTest::addColumn<bool>("prepared");
    QTest::newRow("selection") << false << false;
    QTest::newRow("document") << true << false;
    QTest::newRow("prepared-selection") << false << true;
    QTest::newRow("prepared-document") << true << true;
}

void SpeechControllerTest::realViewerIndexMarks()
{
    QFETCH(bool, continuous);
    QFETCH(bool, prepared);
    const QString module = qEnvironmentVariable("ARIANNA_TEST_SPEECH_MODULE");
    if (module.isEmpty()) QSKIP("Set ARIANNA_TEST_SPEECH_MODULE to test real EPUB word tracking.");
    if (realSpeechAddress.isEmpty()) qunsetenv("SPEECHD_ADDRESS");
    else qputenv("SPEECHD_ADDRESS", realSpeechAddress);
    QTemporaryDir dir;
    KZip zip(dir.filePath(QStringLiteral("speech.epub")));
    QVERIFY(zip.open(QIODevice::WriteOnly));
    QVERIFY(zip.writeFile(QStringLiteral("mimetype"), "application/epub+zip"));
    QVERIFY(zip.writeFile(QStringLiteral("META-INF/container.xml"),
        R"(<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"));
    QVERIFY(zip.writeFile(QStringLiteral("content.opf"),
        R"(<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">speech-real-test</dc:identifier><dc:title>Speech</dc:title><dc:language>de</dc:language></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/><item id="next" href="next.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/><itemref idref="next"/></spine></package>)"));
    QByteArray chapter = R"(<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Speech</title></head><body><p id="selection" lang="de">Diese <em>Wörter</em> werden deutlich vorgelesen. <span xml:lang="en">Hello <b>world</b> this is clear speech.</span></p></body></html>)";
    if (prepared) chapter.replace(" <span xml:lang=", " Diese <b>Wörter</b> werden deutlich vorgelesen. Diese Wörter werden deutlich vorgelesen. <span xml:lang=");
    QVERIFY(zip.writeFile(QStringLiteral("chapter.xhtml"), chapter));
    QVERIFY(zip.writeFile(QStringLiteral("next.xhtml"),
        R"(<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Next</title></head><body lang="de"><p>Weiter geht es. <span xml:lang="en">Reading is finished.</span></p></body></html>)"));
    QVERIFY(zip.close());
    QFile file(dir.filePath(QStringLiteral("speech.epub")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString url = QStringLiteral("data:application/epub+zip;base64,") + QString::fromLatin1(file.readAll().toBase64());

    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), QStringLiteral("http://127.0.0.1"));
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), QStringLiteral("speech-test"));
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import org.kde.kirigami as Kirigami
        import org.kde.arianna
        Kirigami.ApplicationWindow {
            width: 1400; height: 850; visible: true
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
                    entry: ({uniqueIdentifier: ""})
                    property bool scriptDone: false
                    property var scriptResult
                }
            }
        }
    )", QUrl());
    QTRY_VERIFY(component.status() != QQmlComponent::Loading);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *page = object->findChild<QQuickItem *>(QStringLiteral("reader"));
    QVERIFY(page);
    auto *speech = page->findChild<SpeechController *>(QStringLiteral("epubSpeechController"));
    auto *startAction = page->findChild<QObject *>(QStringLiteral("epubSpeechStartAction"));
    QVERIFY(speech && startAction);
    QTRY_VERIFY(!evaluate(startAction, QStringLiteral("applicationWindow().pageStack.layers.busy")).toBool());
    QSignalSpy started(speech, &SpeechController::segmentStarted);
    QSignalSpy reached(speech, &SpeechController::indexMarkReached);
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(startAction, QStringLiteral("view.readerReady")).toBool(), 15000);
    page->setProperty("url", QStringLiteral("speech.epub"));
    evaluate(startAction, QStringLiteral("view.runJavaScript(%1)").arg(encoded(QStringLiteral("openSync(%1, null, '', {})").arg(encoded(url)))));
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(startAction, QStringLiteral("view.bookReady")).toBool(), 15000);
    const auto script = [&](const QString &code) {
        evaluate(startAction, QStringLiteral("root.scriptDone = false; view.runJavaScript(%1, result => {root.scriptResult = result; root.scriptDone = true})").arg(encoded(code)));
        QElapsedTimer timer;
        timer.start();
        while (!page->property("scriptDone").toBool() && timer.elapsed() < 5000) QTest::qWait(10);
        if (!page->property("scriptDone").toBool()) QTest::qFail("Reader script timed out", __FILE__, __LINE__);
        return page->property("scriptResult");
    };
    script(QStringLiteral(R"JS((() => {
        const {doc} = reader.view.renderer.getContents()[0];
        const originalDOM = new WeakMap([[doc, doc.body.innerHTML]]);
        globalThis.liveSpeechResults = [];
        const original = reader.showSpeechMark.bind(reader);
        globalThis.rawSpeechMark = original;
        globalThis.rawSpeechSegment = reader.showSpeechSegment.bind(reader);
        reader.showSpeechMark = (session, id, mark) => {
            const {index, anchor} = reader.view.resolveCFI(mark.cfi, reader.view.cfiFilter);
            const {doc, overlayer} = reader.view.renderer.getContents().find(item => item.index === index);
            if (!originalDOM.has(doc)) originalDOM.set(doc, doc.body.innerHTML);
            original(session, id, mark);
            const drawn = overlayer.element.querySelector('[data-speech-segment]');
            const range = anchor(doc);
            const expected = Array.from(range.getClientRects());
            const rects = Array.from(drawn?.querySelectorAll('rect') ?? []);
            liveSpeechResults.push({id, start: mark.start, length: mark.length, word: range.toString(),
                correct: drawn?.dataset.speechSegment === id
                    && Number(drawn.dataset.speechWordStart) === mark.start
                    && Number(drawn.dataset.speechWordLength) === mark.length
                    && rects.length > 0 && rects.length === expected.length
                    && rects.every((rect, i) => Math.abs(Number(rect.getAttribute('x')) - expected[i].left) < 0.1
                        && Math.abs(Number(rect.getAttribute('y')) - (expected[i].bottom - 2)) < 0.1
                        && Math.abs(Number(rect.getAttribute('width')) - expected[i].width) < 0.1
                        && Number(rect.getAttribute('height')) === 2)
                    && doc.body.innerHTML === originalDOM.get(doc)});
        };
        const p = doc.getElementById('selection');
        const range = doc.createRange(); range.selectNodeContents(p);
        p.dispatchEvent(new PointerEvent('pointerdown', {bubbles: true}));
        doc.getSelection().removeAllRanges(); doc.getSelection().addRange(range);
        p.dispatchEvent(new PointerEvent('pointerup', {bubbles: true}));
    })())JS"));
    QTRY_VERIFY(startAction->property("enabled").toBool());
    const auto segments = evaluate(startAction, QStringLiteral("root.speechSelection")).toList();
    const qsizetype germanSegments = prepared ? 3 : 1;
    QCOMPARE(segments.size(), germanSegments + 1);
    QCOMPARE(segments.at(0).toMap().value(QStringLiteral("lang")).toString(), QStringLiteral("de"));
    QCOMPARE(segments.at(germanSegments).toMap().value(QStringLiteral("lang")).toString(), QStringLiteral("en"));
    qsizetype expectedMarks = 0;
    for (const auto &value : segments) expectedMarks += value.toMap().value(QStringLiteral("marks")).toList().size();
    QCOMPARE(expectedMarks, prepared ? 21 : 11);
    const auto pendingMarkApplied = script(QStringLiteral(R"JS((() => {
        const segment = root.speechSelection[0];
        const mark = segment.marks[0];
        const session = 'pending-word-mark-test';
        reader.setSpeechSession(session);
        rawSpeechMark(session, segment.id, mark);
        rawSpeechSegment(session, segment);
        const content = reader.view.renderer.getContents().find(item => item.index === segment.sectionIndex);
        const underline = content.overlayer.element.querySelector('[data-speech-word-start]');
        const result = underline?.dataset.speechSegment === segment.id
            && underline.dataset.speechWordStart === String(mark.start)
            && underline.dataset.speechWordLength === String(mark.length);
        reader.setSpeechSession('');
        return result;
    })())JS")).toBool();
    QVERIFY(pendingMarkApplied);
    // Automatic voice selection must change voices when xml:lang changes.
    QCOMPARE(Config::speechVoice(), QString());
    if (continuous) {
        auto *menuItem = page->findChild<QQuickItem *>(QStringLiteral("epubReadSelectionMenuItem"));
        QVERIFY(menuItem);
        QTRY_VERIFY(evaluate(startAction, QStringLiteral("selectionPopup.visible")).toBool());
        clickButton(menuItem, Qt::ControlModifier);
        expectedMarks += 6;
    } else {
        evaluate(startAction, QStringLiteral("selectionPopup.close()"));
        QTRY_VERIFY(!evaluate(startAction, QStringLiteral("selectionPopup.visible")).toBool());
        QVERIFY(QMetaObject::invokeMethod(startAction, "trigger"));
    }
    QTRY_VERIFY_WITH_TIMEOUT(!started.isEmpty() || !speech->errorString().isEmpty(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(speech->state() == SpeechController::Idle || !speech->errorString().isEmpty(), 30000);
    QVERIFY2(speech->errorString().isEmpty(), qPrintable(speech->errorString()));
    QCOMPARE(reached.size(), expectedMarks);
    QCOMPARE(started.size(), germanSegments + (continuous ? 3 : 1));
    if (continuous) {
        QVERIFY(!page->property("continuousSpeech").toBool());
        QCOMPARE(script(QStringLiteral("reader.view.renderer.getContents()[0].index")).toInt(), 1);
        QCOMPARE(started.at(germanSegments + 1).at(1).toMap().value(QStringLiteral("text")).toString().trimmed(), QStringLiteral("Weiter geht es."));
        QCOMPARE(started.at(germanSegments + 2).at(1).toMap().value(QStringLiteral("text")).toString().trimmed(), QStringLiteral("Reading is finished."));
    }
    const auto highlighted = script(QStringLiteral("liveSpeechResults")).toList();
    QCOMPARE(highlighted.size(), expectedMarks);
    qsizetype index = 0;
    for (const auto &event : started) {
        const auto segment = event.at(1).toMap();
        const auto text = segment.value(QStringLiteral("text")).toString();
        for (const auto &word : segment.value(QStringLiteral("marks")).toList()) {
            const auto mark = word.toMap();
            const auto highlight = highlighted.at(index).toMap();
            QCOMPARE(reached.at(index).at(0).toString(), speech->sessionId());
            QCOMPARE(reached.at(index).at(1).toString(), segment.value(QStringLiteral("id")).toString());
            QCOMPARE(reached.at(index).at(2).toMap(), mark);
            QCOMPARE(highlight.value(QStringLiteral("id")), segment.value(QStringLiteral("id")));
            QCOMPARE(highlight.value(QStringLiteral("word")).toString(), text.mid(mark.value(QStringLiteral("start")).toInt(), mark.value(QStringLiteral("length")).toInt()));
            QVERIFY2(highlight.value(QStringLiteral("correct")).toBool(), qPrintable(highlight.value(QStringLiteral("word")).toString()));
            ++index;
        }
    }
    QCOMPARE(script(QStringLiteral("reader.view.renderer.getContents()[0].overlayer.element.querySelectorAll('[data-speech-segment]').length")).toInt(), 0);
    qInfo().noquote() << "Real EPUB word underlines:" << expectedMarks << "with automatic de/en voice switching, continuous:" << continuous;
}

void SpeechControllerTest::voiceSelectionAndErrors()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QSignalSpy errors(&speech, &SpeechController::errorOccurred);
    speech.refreshVoices();
    QTRY_COMPARE(speech.voices().size(), 2);
    Config::setSpeechVoice(speech.voices().last().toMap().value(QStringLiteral("id")).toString());
    speech.speak(QStringLiteral("Manuelle Stimme"), QStringLiteral("de"));
    QTRY_COMPARE(server.messages().size(), 1);
    QCOMPARE(server.messages().last().language, QStringLiteral("en"));
    server.finish(server.messages().last().id);
    QTRY_COMPARE(speech.state(), SpeechController::Idle);
    Config::setSpeechVoice(QStringLiteral("missing voice"));
    speech.speak(QStringLiteral("Text"));
    QTRY_COMPARE(errors.size(), 1);
    QCOMPARE(speech.state(), SpeechController::Idle);
    QCOMPARE(server.messages().size(), 1);
    Config::setSpeechVoice(QString());
    speech.speak(QStringLiteral("धर्मः"), QStringLiteral("sa"));
    QTRY_COMPARE(errors.size(), 2);
    QCOMPARE(server.messages().size(), 1);
    server.rejectSpeech(true);
    speech.speak(QStringLiteral("Text"), QStringLiteral("de"));
    QTRY_COMPARE(errors.size(), 3);
    QCOMPARE(speech.state(), SpeechController::Idle);
    QVERIFY(!speech.errorString().isEmpty());
    server.rejectSpeech(false);
    speech.speak(QStringLiteral("Recovered"), QStringLiteral("en"));
    QTRY_COMPARE(server.messages().size(), 2);
    QVERIFY(speech.errorString().isEmpty());
}

void SpeechControllerTest::reconnectAndCancel()
{
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")));
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    SpeechController speech;
    QSignalSpy errors(&speech, &SpeechController::errorOccurred);
    speech.speak(QStringLiteral("First"), QStringLiteral("en"));
    QTRY_COMPARE(server.messages().size(), 1);
    server.disconnectClients();
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 4000);
    QCOMPARE(speech.state(), SpeechController::Idle);
    speech.speak(QStringLiteral("Second"), QStringLiteral("en"));
    QTRY_COMPARE(server.messages().size(), 2);
    speech.stop();
    QTRY_VERIFY(server.commands().contains(QStringLiteral("CANCEL SELF")));
    // Stopping a queued request must not start speaking it later.
    speech.speak(QStringLiteral("Obsolete"), QStringLiteral("en"));
    speech.stop();
    QTest::qWait(200);
    QCOMPARE(speech.state(), SpeechController::Idle);
    // A second reader's destruction cancels only its own connection.
    {
        SpeechController second;
        second.speak(QStringLiteral("Other reader"), QStringLiteral("en"));
        QTRY_VERIFY(server.messages().last().text == QStringLiteral("Other reader"));
    }
    QCOMPARE(server.commands().last(), QStringLiteral("CANCEL SELF"));
}

void SpeechControllerTest::pdfContinuousReading_data()
{
    QTest::addColumn<QString>("activation");
    QTest::addColumn<QString>("module");
    QTest::newRow("ctrl-click") << QStringLiteral("click") << QStringLiteral("piper");
    QTest::newRow("ctrl-return") << QStringLiteral("return") << QStringLiteral("piper");
    QTest::newRow("ctrl-space-fallback") << QStringLiteral("space") << QStringLiteral("test");
}

void SpeechControllerTest::pdfContinuousReading()
{
    QFETCH(QString, activation);
    QFETCH(QString, module);
    const bool prepared = module == QLatin1String("piper");
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), false, module);
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    const QString path = dir.filePath(QStringLiteral("continuous.pdf"));
    {
        QPdfWriter writer(path);
        writer.setResolution(72);
        QPainter painter(&writer);
        painter.drawText(70, 60, QStringLiteral("Nur diese Auswahl."));
        painter.drawText(70, 160, QStringLiteral("Nur diese Auswahl."));
        painter.drawText(70, 200, QStringLiteral("Danach geht es"));
        painter.drawText(70, 220, QStringLiteral("weiter."));
        QVERIFY(writer.newPage()); // A scanned/textless page must not end the stream.
        QVERIFY(writer.newPage());
        for (int i = 1; i <= 27; ++i)
            painter.drawText(70, 80 + 20 * i, QStringLiteral("Folgesatz %1 endet hier.").arg(i));
    }
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray original = file.readAll();
    file.close();
    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), QStringLiteral("http://127.0.0.1"));
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), QStringLiteral("speech-test"));
    QQmlComponent component(&engine);
    component.setData(R"QML(
        import QtQuick
        import org.kde.kirigami as Kirigami
        import org.kde.arianna
        Kirigami.ApplicationWindow {
            width: 1400; height: 850; visible: true
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
            pageStack.initialPage: Kirigami.Page {}
            Component.onCompleted: pageStack.layers.push(readerComponent)
            Component {
                id: readerComponent
                PdfViewerPage { objectName: "reader"; entry: ({uniqueIdentifier: ""}) }
            }
        }
    )QML", QUrl());
    QTRY_VERIFY(component.status() != QQmlComponent::Loading);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *page = object->findChild<QQuickItem *>(QStringLiteral("reader"));
    QVERIFY(page);
    auto *speech = page->findChild<SpeechController *>(QStringLiteral("pdfSpeechController"));
    auto *controls = page->findChild<QQuickWindow *>(QStringLiteral("pdfSpeechControlWindow"));
    auto *start = page->findChild<QObject *>(QStringLiteral("pdfSpeechStartAction"));
    auto *mode = page->findChild<QObject *>(QStringLiteral("pdfSpeechModeAction"));
    auto *readItem = page->findChild<QQuickItem *>(QStringLiteral("pdfReadSelectionMenuItem"));
    auto *document = page->findChild<OkularPdfDocument *>();
    QVERIFY(speech && controls && start && mode && readItem && document);
    QTRY_VERIFY(!evaluate(start, QStringLiteral("applicationWindow().pageStack.layers.busy")).toBool());
    page->setProperty("filename", path);
    page->setProperty("url", QUrl::fromLocalFile(path).toString());
    QTRY_VERIFY(document->opened());
    QCOMPARE(document->pageCount(), 3);
    const auto select = [&] {
        auto area = std::make_unique<Okular::RegularAreaRect>();
        area->append(Okular::NormalizedRect(0.05, 0.18, 0.95, 0.24));
        document->setTextSelection(0, std::move(area));
        evaluate(start, QStringLiteral("view.openSelectionMenu(100, 100)"));
    };
    select();
    QCOMPARE(document->selectedTextForTranslation(), QStringLiteral("Nur diese Auswahl."));
    const auto position = document->speechSelectionRange();
    QCOMPARE(position.value(QStringLiteral("page")).toInt(), 0);
    QVERIFY(position.value(QStringLiteral("offset")).toInt() >= QStringLiteral("Nur diese Auswahl.").size());
    // Reading extraction does not change the live selection or reading page.
    const auto selectedGeometry = document->textSelectionGeometry();
    const auto firstBatch = document->speechPageSegments(0, position.value(QStringLiteral("offset")).toInt());
    QCOMPARE(firstBatch.value(QStringLiteral("segments")).toList().size(), 2);
    QCOMPARE(firstBatch.value(QStringLiteral("segments")).toList().last().toMap().value(QStringLiteral("text")).toString(),
        QStringLiteral("Danach geht es weiter."));
    QCOMPARE(document->textSelectionGeometry(), selectedGeometry);
    QCOMPARE(document->currentPage(), 0);
    QVERIFY(document->speechPageSegments(1, 0).value(QStringLiteral("segments")).toList().isEmpty());
    const auto manySentences = document->speechPageSegments(2, 0);
    QCOMPARE(manySentences.value(QStringLiteral("segments")).toList().size(), 24);
    QVERIFY(!manySentences.value(QStringLiteral("endOfPage")).toBool());
    QCOMPARE(document->speechPageSegments(2, manySentences.value(QStringLiteral("nextOffset")).toInt())
        .value(QStringLiteral("segments")).toList().size(), 3);
    QSignalSpy started(speech, &SpeechController::segmentStarted);
    const auto activate = [&] {
        if (activation == QLatin1String("click")) {
            clickButton(readItem, Qt::ControlModifier);
        } else {
            window->requestActivate();
            readItem->forceActiveFocus();
            QTest::keyClick(window, activation == QLatin1String("return") ? Qt::Key_Return : Qt::Key_Space, Qt::ControlModifier);
        }
    };
    QTRY_VERIFY(readItem->isVisible() && readItem->isEnabled());
    activate();
    QTRY_COMPARE(server.messages().size(), 1);
    QTRY_VERIFY(controls->isVisible());
    QCOMPARE(page->property("speechMode").toInt(), 2);
    QVERIFY(!speech->repeatSelection());
    QVERIFY(!document->hasTextSelection()); // Menu closes normally, position is retained separately.
    QVERIFY(server.messages().first().text.startsWith(QStringLiteral("Nur diese Auswahl.")));
    QCOMPARE(server.messages().first().text.count(QStringLiteral("Nur diese Auswahl.")), 1);
    if (prepared) {
        QCOMPARE(server.messages().first().rawText.count(QStringLiteral("<s>")), 2);
        QCOMPARE(server.messages().first().rawText.count(QStringLiteral("<break")), 1);
    }
    // Prefetching later pages cannot move the visible document ahead of audio.
    QTest::qWait(100);
    QCOMPARE(document->currentPage(), 0);
    speech->pause();
    QTRY_COMPARE(speech->state(), SpeechController::Paused);
    speech->resume();
    QTRY_COMPARE(speech->state(), SpeechController::Speaking);
    // Piper cancels a prepared batch on Pause and restarts its current sentence.
    QTRY_COMPARE(server.messages().size(), prepared ? 2 : 1);
    QString spoken;
    qsizetype processed = prepared ? 1 : 0;
    for (int guard = 0; guard < 35 && speech->state() != SpeechController::Idle; ++guard) {
        QTRY_VERIFY(server.messages().size() > processed || speech->state() == SpeechController::Idle);
        if (speech->state() == SpeechController::Idle) break;
        const auto message = server.messages().at(processed++);
        spoken += message.text;
        for (const auto &mark : message.marks)
            server.sendIndexMark(message.id, message.client, mark.name);
        server.finish(message.id);
    }
    QTRY_COMPARE(speech->state(), SpeechController::Idle);
    QCOMPARE(processed, server.messages().size());
    QSet<QString> spokenSegments;
    for (const auto &event : started)
        spokenSegments.insert(event.at(1).toMap().value(QStringLiteral("id")).toString());
    QCOMPARE(spokenSegments.size(), 29); // Resume may repeat BEGIN for the current segment.
    QCOMPARE(spoken.count(QStringLiteral("Nur diese Auswahl.")), 1);
    QCOMPARE(spoken.count(QStringLiteral("Danach geht es weiter.")), 1);
    for (int i = 1; i <= 27; ++i)
        QCOMPARE(spoken.count(QStringLiteral("Folgesatz %1 endet hier.").arg(i)), 1);
    QCOMPARE(document->currentPage(), 2);
    QVERIFY(!page->property("continuousSpeech").toBool());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);
    file.close();

    // Ordinary selection reading also sends wrapped lines as a single utterance.
    evaluate(start, QStringLiteral("view.goToPage(0); root.setSpeechMode(0)"));
    auto lines = std::make_unique<Okular::RegularAreaRect>();
    lines->append(Okular::NormalizedRect(0.05, 0.18, 0.95, 0.31));
    document->setTextSelection(0, std::move(lines));
    const QString sourceSelection = document->selectedText();
    QVERIFY(sourceSelection.contains(QLatin1Char('\n')));
    QCOMPARE(document->selectedTextForSpeech(), QStringLiteral("Nur diese Auswahl. Danach geht es weiter."));
    QCOMPARE(document->selectedText(), sourceSelection);
    evaluate(start, QStringLiteral("view.openSelectionMenu(100, 100)"));
    QTRY_VERIFY(readItem->isVisible() && readItem->isEnabled());
    const int lineStart = server.messages().size();
    clickButton(readItem);
    QTRY_COMPARE(server.messages().size(), lineStart + 1);
    QCOMPARE(server.messages().last().text, QStringLiteral("Nur diese Auswahl. Danach geht es weiter."));
    server.finish(server.messages().last().id);
    QTRY_COMPARE(speech->state(), SpeechController::Idle);

    // Continuous -> Once -> Repeat retains just the active logical sentence.
    evaluate(start, QStringLiteral("view.goToPage(0)"));
    select();
    QTRY_VERIFY(readItem->isVisible() && readItem->isEnabled());
    const int before = server.messages().size();
    activate();
    QTRY_COMPARE(server.messages().size(), before + 1);
    const auto active = server.messages().last();
    QVERIFY(QMetaObject::invokeMethod(mode, "trigger"));
    QCOMPARE(page->property("speechMode").toInt(), 0);
    QVERIFY(!page->property("continuousSpeech").toBool());
    QVERIFY(QMetaObject::invokeMethod(mode, "trigger"));
    QCOMPARE(page->property("speechMode").toInt(), 1);
    // Prepared playback stops at the next sentence mark; other modules stop at END.
    if (prepared) {
        QVERIFY(active.marks.size() >= 2);
        server.sendIndexMark(active.id, active.client, active.marks.at(1).name);
    } else {
        server.finish(active.id);
    }
    QTRY_COMPARE(server.messages().size(), before + 2);
    QCOMPARE(server.messages().last().text.trimmed(), QStringLiteral("Nur diese Auswahl."));
    server.finish(server.messages().last().id);
    QTRY_COMPARE(server.messages().size(), before + 3);
    QCOMPARE(server.messages().last().text.trimmed(), QStringLiteral("Nur diese Auswahl."));
    speech->pause();
    QTRY_COMPARE(speech->state(), SpeechController::Paused);
    const auto cancelled = server.messages().last();
    controls->close();
    QTRY_COMPARE(speech->state(), SpeechController::Idle);
    server.sendOldBegin(cancelled.id, cancelled.client);
    server.sendOldEnd(cancelled.id, cancelled.client);
    QTest::qWait(100);
    QCOMPARE(server.messages().size(), before + 3);

    // Promotion appends after the captured selection without restarting it.
    evaluate(start, QStringLiteral("root.setSpeechMode(0)"));
    select();
    QTRY_VERIFY(readItem->isVisible() && readItem->isEnabled());
    clickButton(readItem);
    QTRY_COMPARE(server.messages().size(), before + 4);
    QCOMPARE(server.messages().last().text.trimmed(), QStringLiteral("Nur diese Auswahl."));
    const auto selection = server.messages().last();
    const QString session = speech->sessionId();
    QVERIFY(QMetaObject::invokeMethod(mode, "trigger"));
    QVERIFY(QMetaObject::invokeMethod(mode, "trigger"));
    QTRY_VERIFY(page->property("continuousSpeech").toBool());
    QCOMPARE(speech->sessionId(), session);
    QCOMPARE(server.messages().size(), before + 4);
    server.finish(selection.id);
    QTRY_COMPARE(server.messages().size(), before + 5);
    QVERIFY(server.messages().last().text.startsWith(QStringLiteral("Danach geht es weiter.")));
    controls->close();
    QTRY_COMPARE(speech->state(), SpeechController::Idle);
    QVERIFY(!page->property("continuousSpeech").toBool());

    // Close before Qt.callLater extracts the initial page, then reject the delayed callback.
    const int messages = server.messages().size();
    evaluate(start, QStringLiteral("root.readDocumentFrom({page: 0, offset: 0}); speechControls.close()"));
    QTest::qWait(100);
    QCOMPARE(server.messages().size(), messages);
    QCOMPARE(speech->state(), SpeechController::Idle);
    QVERIFY(!page->property("continuousSpeech").toBool());
    evaluate(start, QStringLiteral("root.readDocumentFrom({page: 0, offset: 0}); root.url = ''"));
    QTest::qWait(100);
    QCOMPARE(server.messages().size(), messages);
    QCOMPARE(speech->state(), SpeechController::Idle);
    QVERIFY(!controls->isVisible());
}

void SpeechControllerTest::viewerControls_data()
{
    QTest::addColumn<QString>("format");
    QTest::addColumn<bool>("useMenu");
    QTest::addColumn<bool>("continuous");
    QTest::addColumn<bool>("math");
    QTest::addColumn<bool>("paragraphs");
    QTest::addColumn<bool>("mathPackets");
    QTest::addColumn<bool>("complexMath");
    QTest::addColumn<bool>("sanskrit");
    QTest::addColumn<bool>("repeat");
    QTest::newRow("epub-controls") << QStringLiteral("epub") << false << false << false << false << false << false << false << false;
    QTest::newRow("pdf-controls") << QStringLiteral("pdf") << false << false << false << false << false << false << false << false;
    QTest::newRow("epub-menu") << QStringLiteral("epub") << true << false << false << false << false << false << false << false;
    QTest::newRow("pdf-menu") << QStringLiteral("pdf") << true << false << false << false << false << false << false << false;
    QTest::newRow("epub-document") << QStringLiteral("epub") << true << true << false << false << false << false << false << false;
    QTest::newRow("epub-math-selection") << QStringLiteral("epub") << true << false << true << false << false << false << false << false;
    QTest::newRow("epub-math-document") << QStringLiteral("epub") << true << true << true << false << false << false << false << false;
    QTest::newRow("epub-paragraph-selection") << QStringLiteral("epub") << false << false << false << true << false << false << false << false;
    QTest::newRow("epub-paragraph-document") << QStringLiteral("epub") << true << true << false << true << false << false << false << false;
    QTest::newRow("epub-inline-math-selection") << QStringLiteral("epub") << true << false << true << false << true << false << false << false;
    QTest::newRow("epub-inline-math-document") << QStringLiteral("epub") << true << true << true << false << true << false << false << false;
    QTest::newRow("epub-complex-inline-math") << QStringLiteral("epub") << true << false << true << false << true << true << false << false;
    QTest::newRow("epub-sanskrit-selection") << QStringLiteral("epub") << true << false << false << false << false << false << true << false;
    QTest::newRow("epub-sanskrit-document") << QStringLiteral("epub") << true << true << false << false << false << false << true << false;
    QTest::newRow("epub-repeat") << QStringLiteral("epub") << false << false << false << false << false << false << false << true;
    QTest::newRow("pdf-repeat") << QStringLiteral("pdf") << false << false << false << false << false << false << false << true;
    QTest::newRow("epub-crossref-de") << QStringLiteral("epub") << true << false << false << false << false << false << false << false;
    QTest::newRow("epub-crossref-en") << QStringLiteral("epub") << true << false << false << false << false << false << false << false;
    QTest::newRow("epub-crossref-document") << QStringLiteral("epub") << true << true << false << false << false << false << false << false;
    QTest::newRow("epub-crossref-repeat") << QStringLiteral("epub") << true << false << false << false << false << false << false << true;
    QTest::newRow("epub-toggle-once") << QStringLiteral("epub") << true << false << false << false << false << false << false << false;
    QTest::newRow("epub-toggle-repeat") << QStringLiteral("epub") << true << false << false << false << false << false << false << false;
}

void SpeechControllerTest::viewerControls()
{
    QFETCH(QString, format);
    QFETCH(bool, useMenu);
    QFETCH(bool, continuous);
    QFETCH(bool, math);
    QFETCH(bool, paragraphs);
    QFETCH(bool, mathPackets);
    QFETCH(bool, complexMath);
    QFETCH(bool, sanskrit);
    QFETCH(bool, repeat);
    const bool epub = format == QLatin1String("epub");
    const bool crossrefs = QByteArray(QTest::currentDataTag()).startsWith("epub-crossref-");
    const bool englishReferences = QByteArray(QTest::currentDataTag()) == "epub-crossref-en";
    const bool modeToggle = QByteArray(QTest::currentDataTag()).startsWith("epub-toggle-");
    const bool sentenceRepeat = QByteArray(QTest::currentDataTag()) == "epub-toggle-repeat";
    QTemporaryDir dir;
    FakeSpeechDispatcher server(dir.filePath(QStringLiteral("speech.sock")), false,
        paragraphs || mathPackets || sanskrit || modeToggle || crossrefs ? QStringLiteral("piper") : QStringLiteral("test"), sanskrit);
    QVERIFY(server.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    QHttpServer metadata;
    QTcpServer listener;
    QByteArray epubBytes;
    int bookReads = 0;
    int referenceReads = 0;
    QJsonArray referenceBooks{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("gita")}, {QStringLiteral("title"), QStringLiteral("भगवद्गीता")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("bhagavatam")}, {QStringLiteral("title"), QStringLiteral("ŚRĪMAD-BHĀGAVATAM")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("algebra")}, {QStringLiteral("title"), QStringLiteral("ALGEBRA II")}}};
    if (crossrefs) {
        QVERIFY(metadata.route(QStringLiteral("/books"), QHttpServerRequest::Method::Get, [&] {
            ++bookReads;
            return QHttpServerResponse(QJsonObject{{QStringLiteral("books"), referenceBooks}});
        }));
        QVERIFY(metadata.route(QStringLiteral("/speech-test/refs"), QHttpServerRequest::Method::Get, [&] {
            ++referenceReads;
            return QHttpServerResponse(QJsonObject{{QStringLiteral("bookId"), QStringLiteral("speech-test")}, {QStringLiteral("refs"), QJsonArray{
                QJsonObject{{QStringLiteral("sourceAnchorId"), QStringLiteral("chapter-ref")}, {QStringLiteral("sourceAnchorTitle"), QStringLiteral("B.G. Kapitel 2")}, {QStringLiteral("targetBookId"), QStringLiteral("gita")}},
                QJsonObject{{QStringLiteral("sourceAnchorId"), QStringLiteral("verse-ref")}, {QStringLiteral("sourceAnchorTitle"), QStringLiteral("S.B. Vers 1.2.3")}, {QStringLiteral("targetBookId"), QStringLiteral("bhagavatam")}},
                QJsonObject{{QStringLiteral("sourceAnchorId"), QStringLiteral("equation-ref")}, {QStringLiteral("sourceAnchorTitle"), QStringLiteral("A. Gleichung (4.7)")}, {QStringLiteral("targetBookId"), QStringLiteral("algebra")}}}}});
        }));
        QVERIFY(metadata.route(QStringLiteral("/speech-test/book.epub"), QHttpServerRequest::Method::Get, [&] {
            return QHttpServerResponse(QByteArrayLiteral("application/epub+zip"), epubBytes);
        }));
        QVERIFY(metadata.route(QStringLiteral("/books"), QHttpServerRequest::Method::Options, [] { return QHttpServerResponse(QByteArray{}); }));
        QVERIFY(metadata.route(QStringLiteral("/speech-test/refs"), QHttpServerRequest::Method::Options, [] { return QHttpServerResponse(QByteArray{}); }));
        metadata.addAfterRequestHandler(&metadata, [](const QHttpServerRequest &, QHttpServerResponse &response) {
            auto headers = response.headers();
            headers.append("Access-Control-Allow-Origin", "*");
            headers.append("Access-Control-Allow-Headers", "X-Arianna-Session-Token");
            headers.append("Access-Control-Allow-Methods", "GET, OPTIONS");
            response.setHeaders(headers);
        });
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        QVERIFY(metadata.bind(&listener));
    }
    const QString metadataUrl = crossrefs ? QStringLiteral("http://127.0.0.1:%1").arg(listener.serverPort()) : QStringLiteral("http://127.0.0.1");
    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), metadataUrl);
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), QStringLiteral("speech-test"));
    QQmlComponent component(&engine);
    component.setData(QStringLiteral(R"(
        import QtQuick
        import org.kde.kirigami as Kirigami
        import org.kde.arianna
        Kirigami.ApplicationWindow {
            width: 1400; height: 850; visible: true
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
            pageStack.initialPage: Kirigami.Page {}
            Component.onCompleted: pageStack.layers.push(readerComponent)
            Component {
                id: readerComponent
                %1ViewerPage {
                objectName: "reader"
                entry: ({uniqueIdentifier: ""})
                property bool scriptDone: false
                property var scriptResult
                }
            }
        }
    )")
                          .arg(epub ? QStringLiteral("Epub") : QStringLiteral("Pdf"))
                          .toUtf8(),
                      QUrl());
    QTRY_VERIFY(component.status() != QQmlComponent::Loading);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *page = object->findChild<QQuickItem *>(QStringLiteral("reader"));
    auto *speech = page->findChild<SpeechController *>(format + QStringLiteral("SpeechController"));
    auto *start = page->findChild<QObject *>(format + QStringLiteral("SpeechStartAction"));
    auto *stop = page->findChild<QObject *>(format + QStringLiteral("SpeechStopAction"));
    auto *pause = page->findChild<QObject *>(format + QStringLiteral("SpeechPauseAction"));
    auto *mode = page->findChild<QObject *>(format + QStringLiteral("SpeechModeAction"));
    auto *rateDown = page->findChild<QObject *>(format + QStringLiteral("SpeechRateDecreaseAction"));
    auto *rateUp = page->findChild<QObject *>(format + QStringLiteral("SpeechRateIncreaseAction"));
    QVERIFY(page && speech && start && stop && pause && mode && rateDown && rateUp);
    QVERIFY(!page->findChild<QObject *>(format + QStringLiteral("SpeechControlsAction")));
    auto *controls = page->findChild<QQuickWindow *>(format + QStringLiteral("SpeechControlWindow"));
    QVERIFY(controls);
    QVERIFY(!controls->isVisible());
    QCOMPARE(controls->modality(), Qt::NonModal);
    QCOMPARE(controls->transientParent(), window);
    QVERIFY(controls->flags().testFlag(Qt::WindowTitleHint));
    QVERIFY(controls->flags().testFlag(Qt::WindowCloseButtonHint));
    QVERIFY(QMetaObject::invokeMethod(controls, "open"));
    QVERIFY(QTest::qWaitForWindowExposed(controls));
    QVERIFY(controls->width() < 500 && controls->height() < 150);
    auto *readMenuItem = page->findChild<QQuickItem *>(format + QStringLiteral("ReadSelectionMenuItem"));
    QVERIFY(readMenuItem);
    QVERIFY(!readMenuItem->isEnabled());
    QCOMPARE(readMenuItem->property("text").toString(), i18nc("@action:inmenu", "Read Aloud"));
    QCOMPARE(evaluate(readMenuItem, QStringLiteral("icon.source.toString()")).toString(),
             QStringLiteral("qrc:/qt/qml/org/kde/arianna/qml/icons/read-aloud.svg"));
    QVERIFY(QFile::exists(QStringLiteral(":/qt/qml/org/kde/arianna/qml/icons/read-aloud.svg")));
    // PDF loading is fast enough to finish before the reader's opening transition.
    QTRY_VERIFY(!evaluate(start, QStringLiteral("applicationWindow().pageStack.layers.busy")).toBool());
    QVERIFY(!start->property("enabled").toBool());
    QVERIFY(!stop->property("enabled").toBool());
    QVERIFY(!pause->property("enabled").toBool());
    // All six controls belong to the separate, non-modal tool window.
    QQuickItem *startButton = nullptr;
    QQuickItem *stopButton = nullptr;
    QQuickItem *pauseButton = nullptr;
    QQuickItem *modeButton = nullptr;
    QQuickItem *rateDownButton = nullptr;
    QQuickItem *rateUpButton = nullptr;
    QTRY_VERIFY((startButton = buttonFor(controls->contentItem(), start)));
    QTRY_VERIFY((stopButton = buttonFor(controls->contentItem(), stop)));
    QTRY_VERIFY((pauseButton = buttonFor(controls->contentItem(), pause)));
    QTRY_VERIFY((modeButton = buttonFor(controls->contentItem(), mode)));
    QTRY_VERIFY((rateDownButton = buttonFor(controls->contentItem(), rateDown)));
    QTRY_VERIFY((rateUpButton = buttonFor(controls->contentItem(), rateUp)));
    auto *rateLabel = controls->findChild<QQuickItem *>(format + QStringLiteral("SpeechRateLabel"));
    QVERIFY(rateLabel);
    QVERIFY(!controls->findChild<QQuickItem *>(format + QStringLiteral("SpeechCloseButton")));
    QCOMPARE(rateLabel->property("text").toString(), QStringLiteral("0%"));
    QVERIFY(!buttonFor(window->contentItem(), start));
    const auto x = [](QQuickItem *item) {
        return item->mapToScene(QPointF()).x();
    };
    QVERIFY(x(startButton) < x(stopButton));
    QVERIFY(x(stopButton) < x(pauseButton));
    QVERIFY(x(pauseButton) < x(modeButton));
    QVERIFY(x(modeButton) < x(rateLabel));
    QVERIFY(x(rateLabel) < x(rateUpButton));
    QCOMPARE(x(rateDownButton), x(rateUpButton));
    QVERIFY(rateUpButton->mapToScene(QPointF()).y() < rateDownButton->mapToScene(QPointF()).y());
    auto *searchField = qobject_cast<QQuickItem *>(evaluate(start, QStringLiteral("root.headerSearchField")).value<QObject *>());
    QVERIFY(searchField);
    clickButton(rateUpButton);
    QCOMPARE(speech->speechRate(), 10);
    QCOMPARE(rateLabel->property("text").toString(), QStringLiteral("+10%"));
    clickButton(rateDownButton);
    QCOMPARE(speech->speechRate(), 0);
    QCOMPARE(rateLabel->property("text").toString(), QStringLiteral("0%"));
    clickButton(rateDownButton);
    QCOMPARE(speech->speechRate(), -10);
    QCOMPARE(rateLabel->property("text").toString(), QStringLiteral("-10%"));
    clickButton(rateUpButton);
    QCOMPARE(speech->speechRate(), 0);
    auto *remarks = evaluate(start, QStringLiteral("root.actions[1]")).value<QObject *>();
    auto *remarksButton = buttonFor(window->contentItem(), remarks);
    QVERIFY(remarksButton);
    QVERIFY(x(remarksButton) < x(searchField));
    QVERIFY(!speech->repeatSelection());
    QVERIFY(!mode->property("checked").toBool());
    QCOMPARE(evaluate(mode, QStringLiteral("icon.name")).toString(), QStringLiteral("media-playlist-normal"));
    const auto clickMode = [&] {
        clickButton(modeButton, Qt::NoModifier);
    };
    clickMode();
    QTRY_VERIFY(speech->repeatSelection());
    QVERIFY(mode->property("checked").toBool());
    QCOMPARE(evaluate(mode, QStringLiteral("icon.name")).toString(), QStringLiteral("media-playlist-repeat"));
    clickMode();
    QTRY_VERIFY(!speech->repeatSelection());
    QCOMPARE(page->property("speechMode").toInt(), 2);
    QVERIFY(modeButton->isEnabled());
    QVERIFY(evaluate(mode, QStringLiteral("icon.source.toString()")).toString().endsWith(QStringLiteral("speech-continuous.svg")));
    clickMode();
    QCOMPARE(page->property("speechMode").toInt(), 0);
    if (repeat) clickMode();
    controls->close();
    QTRY_VERIFY(!controls->isVisible());

    const auto script = [&](const QString &code) {
        evaluate(start, QStringLiteral("root.scriptDone = false; view.runJavaScript(%1, result => {root.scriptResult = result; root.scriptDone = true})").arg(encoded(code)));
        QElapsedTimer timer;
        timer.start();
        while (!page->property("scriptDone").toBool() && timer.elapsed() < 5000)
            QTest::qWait(10);
        if (!page->property("scriptDone").toBool())
            QTest::qFail("Reader script timed out", __FILE__, __LINE__);
        return page->property("scriptResult");
    };

    if (epub) {
        KZip zip(dir.filePath(QStringLiteral("speech.epub")));
        QVERIFY(zip.open(QIODevice::WriteOnly));
        QVERIFY(zip.writeFile(QStringLiteral("mimetype"), "application/epub+zip"));
        QVERIFY(zip.writeFile(
            QStringLiteral("META-INF/container.xml"),
            R"(<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"));
        QVERIFY(zip.writeFile(
            QStringLiteral("content.opf"),
            R"(<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">speech-test</dc:identifier><dc:title>Speech</dc:title><dc:language>de</dc:language></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/><item id="nonlinear" href="nonlinear.xhtml" media-type="application/xhtml+xml"/><item id="empty" href="empty.xhtml" media-type="application/xhtml+xml"/><item id="next" href="next.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/><itemref idref="nonlinear" linear="no"/><itemref idref="empty"/><itemref idref="next"/></spine></package>)"));
        QVERIFY(zip.writeFile(
            QStringLiteral("chapter.xhtml"),
            crossrefs ? QByteArray(R"EPUB(<html xmlns="http://www.w3.org/1999/xhtml"><head><title>References</title></head><body lang="de">
<p>Text vor der Auswahl.</p><p id="selection">Siehe <span id="chapter-ref" data-role="anchor" data-anchor-type="crossref" title="B.G. Kapitel 2"><em>hier</em></span>, <a class="bookref" data-ref="verse-ref" href="#selection" title="S.B. Vers 99">dort</a> und <span id="equation-ref" data-role="anchor" data-anchor-type="crossref" title="A. Gleichung (4.7)">Formel</span>. Zweiter Satz.</p>
<p id="other">Normale <a href="#selection" title="Nicht sprechen">Links</a> und <span id="note" data-role="anchor" data-anchor-type="annotation" title="Notiz">Notizen</span>.</p>
<p style="display:none"><span id="hidden" data-role="anchor" data-anchor-type="crossref" title="H. Vers 9">Geheim</span></p>
<p id="missing"><span id="unknown" data-role="anchor" data-anchor-type="crossref" title="U. Vers 9">unbekannt</span></p>
</body></html>)EPUB")
            : sanskrit ? QByteArray(R"(<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Sanskrit</title></head><body lang="en"><p>Never read the earlier text.</p><section id="selection">
<p>Before धर्मः <em>रक्षति</em>। After.</p>
<p xml:lang="sa-Deva">सत्यं <b>वद</b>॥</p>
<p lang="sa">शान्तिः।</p>
</section></body></html>)")
            : paragraphs ? QByteArray(R"(<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Paragraphs</title></head><body lang="de"><p>Text vor der Auswahl.</p><section id="selection">
<p id="first">Erster Satz. Zweiter <em>Satz</em>. Dritter Satz. Vierter Satz. Fünfter Satz. Sechster Satz.</p>
<h1 id="heading-one">Erste Überschrift</h1><h2 id="heading-two">Zweite Überschrift</h2>
<p id="second">Ein weiterer Absatz ohne Punkt</p><p id="third">Ein neuer Absatz ohne Punkt</p>
<p id="mixed">Guten Tag. <span xml:lang="en">Hello world.</span> Deutsch zurück.</p>
</section></body></html>)")
            : math ? QByteArray(R"(<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Math</title></head><body lang="de">
<p id="selection">Vor <mjx-container id="inline" data-semantic-speech="&lt;mark name=&quot;0&quot;/&gt; x zum Quadrat plus eins" data-semantic-speech-none="x zum Quadrat plus eins" data-semantic-locale="de" style="display:inline-block"><svg xmlns="http://www.w3.org/2000/svg" width="90" height="40"><rect width="90" height="40" fill="none" stroke="black"/><g data-mml-node="mi"><use/></g><g data-mml-node="mn"><use/></g><g data-mml-node="mo"><use/></g><g data-mml-node="mn"><use/></g></svg></mjx-container> nach.</p>
<mjx-container id="display" display="true" data-semantic-speech="a &lt;mark name=&quot;1&quot;/&gt; divided &lt;say-as interpret-as=&quot;character&quot;&gt;by&lt;/say-as&gt; b" data-semantic-locale="en" style="display:block;text-align:center"><svg xmlns="http://www.w3.org/2000/svg" width="120" height="55"><text x="2" y="30">GLYPH DUPLICATE</text></svg><mjx-assistive-mml style="display:none"><math xmlns="http://www.w3.org/1998/Math/MathML"><mfrac><mi>a</mi><mi>b</mi></mfrac></math></mjx-assistive-mml></mjx-container>
<mjx-container style="display:none" data-semantic-speech="HIDDEN FORMULA"/>
<p>Danach fertig.</p></body></html>)")
            : continuous ? QStringLiteral("<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Speech</title></head><body lang='de'><p>Dieser Text liegt vor der Auswahl.</p><p id='selection'>Nur diese Auswahl.</p><p>Danach kommt der Rest.</p><p style='display:none'>Unsichtbarer Text.</p><p>%1 Ende.</p></body></html>").arg(QStringLiteral("Lesefortschritt ").repeated(400)).toUtf8()
            : QByteArray(R"(<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Speech</title></head><body><p id="selection" lang="de">Nur diese Auswahl.</p><p>Dieser Text darf nicht vorgelesen werden.</p>
<p id="mixed" lang="de">Davor Deutsch   <em><span id="note-anchor" data-role="anchor" data-anchor-type="annotation">eins Śaṅkarācārya 😀</span></em> <span xml:lang="en">Hello <b>world Śrī Kṛṣṇa</b>&#160;😀</span> zurück e&#x301; danach</p>
<p id="sentences" lang="de">Erster <em>Satz 😀</em>.   Zwei<b>ter</b> Satz kostet 3.14 Euro! „Dritter Satz e&#x301;?“</p>
<p id="danda" xml:lang="sa">धर्मः रक्षति। सत्यं वद॥</p>
</body></html>)")));
        QVERIFY(zip.writeFile(QStringLiteral("nonlinear.xhtml"), "<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Nonlinear</title></head><body><p>Keinesfalls lesen.</p></body></html>"));
        QVERIFY(zip.writeFile(QStringLiteral("empty.xhtml"), "<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Empty</title></head><body></body></html>"));
        QVERIFY(zip.writeFile(QStringLiteral("next.xhtml"), sanskrit
            ? QByteArray("<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Next</title></head><body lang='de'><p>Weiter संस्कृतम्। Fertig.</p></body></html>")
            : math
            ? QByteArray("<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Next</title></head><body lang='en'><mjx-container id='final' display='true' data-semantic-speech='Next formula.' data-semantic-locale='en' style='display:block;text-align:center'><svg xmlns='http://www.w3.org/2000/svg' width='75' height='35'><rect width='75' height='35' fill='none' stroke='black'/><g data-mml-node='mi'><use/></g></svg></mjx-container></body></html>")
            : paragraphs ? QStringLiteral("<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Next</title></head><body lang='de'><p>%1</p></body></html>").arg(QStringLiteral("Weiter geht es. ").repeated(26)).toUtf8()
            : QStringLiteral("<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Next</title></head><body xml:lang='en'>Leading text. <p>%1</p></body></html>").arg(QStringLiteral("Next sentence. ").repeated(26)).toUtf8()));
        QVERIFY(zip.close());
        QFile file(dir.filePath(QStringLiteral("speech.epub")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        epubBytes = file.readAll();
        const QString url = crossrefs ? QString(metadataUrl + QStringLiteral("/speech-test/book.epub"))
            : QString(QStringLiteral("data:application/epub+zip;base64,") + QString::fromLatin1(epubBytes.toBase64()));
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(start, QStringLiteral("view.readerReady")).toBool(), 15000);
        page->setProperty("url", QStringLiteral("speech.epub"));
        const QString open = QStringLiteral("openSync(%1, null, '', {})").arg(encoded(url));
        evaluate(start, QStringLiteral("view.runJavaScript(%1)").arg(encoded(open)));
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(start, QStringLiteral("view.bookReady")).toBool(), 15000);
        if (math && qEnvironmentVariableIsSet("ARIANNA_TEST_MATHJAX_CDN")) {
            script(QStringLiteral(R"JS((() => {
                const doc = reader.view.renderer.getContents()[0].doc;
                const probe = doc.createElementNS('http://www.w3.org/1999/xhtml', 'p');
                probe.id = 'cdn-math-probe'; probe.textContent = '\\(x^2+1\\)'; doc.body.append(probe);
                const display = doc.createElementNS('http://www.w3.org/1999/xhtml', 'div');
                display.id = 'cdn-display-probe'; display.textContent = '$$-a+b+c+d+e$$ $$a+b+c+d+e+f$$'; doc.body.append(display);
                const complex = doc.createElementNS('http://www.w3.org/1999/xhtml', 'p');
                complex.id = 'cdn-complex-probe'; complex.textContent = '\\(a+b+c+d+e+f\\)'; doc.body.append(complex);
                // Let the EPUB's own script create its configuration in the
                // iframe realm, just as a configuration embedded in XHTML does.
                const config = doc.createElementNS('http://www.w3.org/1999/xhtml', 'script');
                config.textContent = `window.MathJax = {options: {
                    sre: {locale:'de'}, menuOptions: {settings:{speech:true,enrich:true}},
                }};`;
                doc.head.append(config);
                globalThis.cdnMathReady = false; globalThis.cdnMathError = '';
                const loader = doc.createElementNS('http://www.w3.org/1999/xhtml', 'script');
                loader.id = 'MathJax-script'; loader.async = true;
                loader.onload = async () => {
                    try {
                        await doc.defaultView.MathJax.startup.promise;
                        await doc.defaultView.MathJax.whenReady(() => {});
                        cdnMathReady = true;
                    } catch (error) { cdnMathError = String(error) }
                };
                loader.onerror = () => cdnMathError = 'MathJax CDN load failed';
                loader.setAttribute('src', 'https://cdn.jsdelivr.net/npm/mathjax@4/tex-svg.js'); doc.head.append(loader);
            })())JS"));
            QTRY_VERIFY_WITH_TIMEOUT(script(QStringLiteral("cdnMathReady || !!cdnMathError")).toBool(), 60000);
            QVERIFY2(script(QStringLiteral("cdnMathError")).toString().isEmpty(), qPrintable(script(QStringLiteral("cdnMathError")).toString()));
            const QString liveFormula = script(QStringLiteral(R"JS((() => {
                const {doc, overlayer} = reader.view.renderer.getContents()[0];
                const probe = doc.getElementById('cdn-math-probe');
                const formula = probe.querySelector('mjx-container');
                const label = formula?.getAttribute('data-semantic-speech-none') || formula?.getAttribute('data-semantic-speech');
                if (!label) return 'No speech label: ' + probe.innerHTML;
                const range = doc.createRange(); range.selectNodeContents(probe);
                const segments = reader.collectSpeechSegments(range, 0);
                if (segments.length !== 1 || segments[0].type !== 'math' || segments[0].text !== label) return 'Wrong speech segment';
                if (!segments[0].inline || segments[0].symbolCount !== 4) return 'Wrong real inline symbol count';
                const complex = doc.getElementById('cdn-complex-probe');
                const complexRange = doc.createRange(); complexRange.selectNodeContents(complex);
                const complexSegments = reader.collectSpeechSegments(complexRange, 0);
                if (complexSegments.length !== 1 || !complexSegments[0].inline
                    || complexSegments[0].symbolCount !== 11 || !complexSegments[0].tooComplex
                    || complexSegments[0].text !== 'zum Sprechen zu komplizierte Formel')
                    return 'Real inline formula bypassed the complexity limit';
                doc.defaultView.MathJax.typesetClear([complex]); complex.remove();
                const display = doc.getElementById('cdn-display-probe');
                const displayRange = doc.createRange(); displayRange.selectNodeContents(display);
                const displayed = reader.collectSpeechSegments(displayRange, 0);
                if (displayed.length !== 2 || displayed.some(s => s.inline)
                    || displayed[0].symbolCount !== 10 || displayed[0].tooComplex
                    || displayed[1].symbolCount !== 11 || !displayed[1].tooComplex
                    || displayed[1].text !== 'zum Sprechen zu komplizierte Formel')
                    return 'Wrong real display complexity limit: ' + JSON.stringify(displayed);
                doc.defaultView.MathJax.typesetClear([display]); display.remove();
                if (segments[0].lang !== 'de') return 'Wrong MathJax speech language: ' + JSON.stringify({
                    lang: segments[0].lang, version: doc.defaultView.MathJax.version,
                    sre: doc.defaultView.MathJax.startup.document.options.sre,
                    settings: doc.defaultView.MathJax.startup.document.menu?.settings,
                    label, attributes: formula.getAttributeNames().map(name => [name, formula.getAttribute(name)]),
                });
                const markup = probe.innerHTML;
                reader.setSpeechSession('cdn-probe'); reader.showSpeechSegment('cdn-probe', segments[0]);
                const rect = overlayer.element.querySelector('[data-speech-type="math"] rect');
                const expected = formula.querySelector('svg').getBoundingClientRect();
                const correct = rect && Math.abs(Number(rect.getAttribute('width')) - expected.width) < 0.1 && probe.innerHTML === markup;
                reader.clearSpeechHighlight();
                doc.defaultView.MathJax.typesetClear([probe]); probe.remove();
                return correct ? '' : 'Wrong formula underline';
            })())JS")).toString();
            QVERIFY2(liveFormula.isEmpty(), qPrintable(liveFormula));
            qInfo() << "Real MathJax CDN output and whole-formula underline verified";
        }
        if (math) {
            const auto content = script(QStringLiteral(R"JS((() => {
                const doc = reader.view.renderer.getContents()[0].doc;
                globalThis.mathSpeechMarkup = () => {
                    const clone = reader.view.renderer.getContents()[0].doc.body.cloneNode(true);
                    // The paginator adjusts SVG max-height on toolbar/viewport
                    // resize. Exclude only that renderer-owned layout property.
                    for (const svg of clone.querySelectorAll('svg')) svg.style.removeProperty('max-height');
                    return clone.innerHTML;
                };
                globalThis.mathSourceDOM = mathSpeechMarkup();
                const range = doc.createRange(); range.selectNodeContents(doc.body);
                return reader.collectSpeechSegments(range, 0).map(s => ({text:s.text.trim(), type:s.type || 'text', lang:s.lang, marks:s.marks.length}));
            })())JS")).toList();
            QCOMPARE(content.size(), 5);
            QCOMPARE(content[0].toMap().value(QStringLiteral("text")).toString(), QStringLiteral("Vor"));
            QCOMPARE(content[1].toMap().value(QStringLiteral("type")).toString(), QStringLiteral("math"));
            QCOMPARE(content[1].toMap().value(QStringLiteral("text")).toString(), QStringLiteral("x zum Quadrat plus eins"));
            QCOMPARE(content[1].toMap().value(QStringLiteral("marks")).toInt(), 0);
            QCOMPARE(content[2].toMap().value(QStringLiteral("text")).toString(), QStringLiteral("nach."));
            QCOMPARE(content[3].toMap().value(QStringLiteral("type")).toString(), QStringLiteral("math"));
            QCOMPARE(content[3].toMap().value(QStringLiteral("lang")).toString(), QStringLiteral("en"));
            QCOMPARE(content[3].toMap().value(QStringLiteral("text")).toString(), QStringLiteral("a divided by b"));
            QCOMPARE(content[4].toMap().value(QStringLiteral("text")).toString(), QStringLiteral("Danach fertig."));
        }
        if (mathPackets) {
            const QString extraction = script(QStringLiteral(R"JS((() => {
                const doc = reader.view.renderer.getContents()[0].doc;
                const section = doc.createElementNS('http://www.w3.org/1999/xhtml', 'section');
                const formula = doc.getElementById('inline').cloneNode(true);
                const paragraph = doc.createElementNS('http://www.w3.org/1999/xhtml', 'p');
                paragraph.append('Vor ', formula, '. Zweiter Satz.'); section.append(paragraph); doc.body.append(section);
                try {
                    const range = doc.createRange(); range.selectNodeContents(paragraph);
                    const parts = reader.collectSpeechSegments(range, 0);
                    if (parts.length !== 4 || parts[0].sentenceId !== parts[1].sentenceId
                        || parts[1].sentenceId !== parts[2].sentenceId || parts[2].sentenceId === parts[3].sentenceId)
                        return 'Formula prevented sentence splitting: ' + JSON.stringify(parts);
                    if (parts[2].text.trim() !== '.' || parts[3].text.trim() !== 'Zweiter Satz.') return 'Wrong clipped prose';
                    for (const part of parts.filter(s => s.type !== 'math')) {
                        for (const mark of part.marks) {
                            const source = reader.view.resolveCFI(mark.cfi, reader.view.cfiFilter).anchor(doc);
                            if (source.toString() !== part.text.slice(mark.start, mark.start + mark.length)) return 'Wrong clipped word CFI';
                        }
                    }
                    const collect = target => {
                        const range = doc.createRange(); range.selectNode(target);
                        return reader.collectSpeechSegments(range, 0)[0];
                    };
                    formula.querySelector('svg').innerHTML = '<g data-mml-node="mrow">'
                        + '<g data-mml-node="mi"><use/></g>'.repeat(10) + '</g>';
                    if (collect(formula).symbolCount !== 10) return 'SVG symbols not counted once';
                    formula.querySelector('svg').innerHTML += '<g data-mml-node="mo"><use/></g>';
                    if (collect(formula).symbolCount !== 11) return 'SVG symbol boundary wrong';
                    if (collect(formula).text !== 'zum Sprechen zu komplizierte Formel' || !collect(formula).tooComplex)
                        return 'Inline formula bypassed the complexity limit';
                    formula.setAttribute('display', 'true');
                    if (collect(formula).text !== 'zum Sprechen zu komplizierte Formel' || !collect(formula).tooComplex)
                        return 'German complexity warning missing';
                    formula.setAttribute('data-semantic-locale', 'en');
                    if (collect(formula).text !== 'too complicated formula to speak') return 'English complexity warning missing';
                    formula.querySelector('svg').lastElementChild.remove();
                    if (collect(formula).text !== 'x zum Quadrat plus eins' || collect(formula).tooComplex)
                        return 'Formula at the ten-symbol limit was rejected';
                    formula.removeAttribute('display');
                    formula.setAttribute('data-semantic-locale', 'de');
                    const mathjax = doc.defaultView.MathJax;
                    doc.defaultView.MathJax = {startup:{document:{math:[{typesetRoot:formula, root:{walkTree:action => {
                        action({kind:'mi',getText:()=> 'x'}); action({kind:'mo',getText:()=> '\u2062'});
                        action({kind:'mn',getText:()=> '123'}); action({kind:'mfrac'});
                    }}}]}}};
                    if (collect(formula).symbolCount !== 3) return 'Internal MathML or invisible operator count wrong';
                    doc.defaultView.MathJax = mathjax;
                    formula.querySelector('svg').remove();
                    formula.innerHTML = '<mjx-math><mjx-mi><mjx-c/></mjx-mi><mjx-mo><mjx-c/></mjx-mo><mjx-mn><mjx-c/></mjx-mn></mjx-math>';
                    if (collect(formula).symbolCount !== 3) return 'CHTML symbol count wrong';
                    formula.innerHTML = '<mjx-assistive-mml><math><mfrac><mi>x</mi><mn>123</mn></mfrac></math></mjx-assistive-mml>';
                    if (collect(formula).symbolCount !== 3) return 'Assistive MathML symbol count wrong';
                    formula.innerHTML = '<svg><rect/></svg>';
                    if (collect(formula).symbolCount !== null) return 'Unknown complexity was guessed';
                    formula.setAttribute('display', 'true');
                    if (collect(formula).text !== 'zum Sprechen zu komplizierte Formel') return 'Unknown display complexity was spoken';
                    return '';
                } finally { section.remove() }
            })())JS")).toString();
            QVERIFY2(extraction.isEmpty(), qPrintable(extraction));
        }
        if (complexMath) script(QStringLiteral(R"JS((() => {
            const doc = reader.view.renderer.getContents()[0].doc;
            doc.getElementById('inline').querySelector('svg').innerHTML += '<g data-mml-node="mi"><use/></g>'.repeat(7);
            mathSourceDOM = mathSpeechMarkup();
        })())JS"));
        if (crossrefs) {
            if (englishReferences) {
                speech->refreshVoices();
                QTRY_VERIFY(!speech->voices().isEmpty());
                for (const auto &value : speech->voices()) {
                    const auto voice = value.toMap();
                    if (voice.value(QStringLiteral("language")).toString() == QLatin1String("en"))
                        Config::setSpeechVoice(voice.value(QStringLiteral("id")).toString());
                }
                QVERIFY(!Config::speechVoice().isEmpty());
                QTRY_COMPARE(evaluate(start, QStringLiteral("root.mathSpeechLocale")).toString(), QStringLiteral("en"));
                evaluate(start, QStringLiteral("root.syncMathSpeechOptions()"));
            }
            script(QStringLiteral("globalThis.referenceSourceDOM = reader.view.renderer.getContents()[0].doc.body.innerHTML"));
        }
        const QString select = QStringLiteral(R"JS((() => {
            const doc = reader.view.renderer.getContents()[0].doc;
            const element = doc.getElementById(%1);
            element.dispatchEvent(new PointerEvent('pointerdown', {bubbles: true}));
            const range = doc.createRange();
            if (element.localName === 'mjx-container') range.selectNode(element);
            else range.selectNodeContents(element);
            doc.getSelection().removeAllRanges(); doc.getSelection().addRange(range);
            element.dispatchEvent(new PointerEvent('pointerup', {bubbles: true}));
        })())JS").arg(encoded(math && !mathPackets ? QStringLiteral("inline") : QStringLiteral("selection")));
        evaluate(start, QStringLiteral("view.runJavaScript(%1)").arg(encoded(select)));
        QTRY_VERIFY(start->property("enabled").toBool());
        if (paragraphs) {
            QVERIFY(script(QStringLiteral(R"JS((() => {
                const content = reader.view.renderer.getContents()[0];
                const range = content.doc.createRange();
                range.selectNodeContents(content.doc.getElementById('selection'));
                const segment = reader.collectSpeechSegments(range, content.index)[0];
                globalThis.wordFallbackSegment = segment;
                reader.setSpeechSession('word-fallback-test');
                reader.showSpeechSegment('word-fallback-test', {...segment, marks: []});
                return true;
            })())JS")).toBool());
            QTest::qWait(450);
            const bool heuristicWordApplied = script(QStringLiteral(R"JS((() => {
                const segment = wordFallbackSegment;
                const content = reader.view.renderer.getContents().find(item => item.index === segment.sectionIndex);
                const underline = content.overlayer.element.querySelector('[data-speech-word-start]');
                const wordStarts = Array.from(new Intl.Segmenter(segment.lang || undefined, {granularity: 'word'}).segment(segment.text))
                    .filter(word => word.isWordLike).map(word => word.index);
                const result = underline?.dataset.speechSegment === segment.id
                    && wordStarts.includes(Number(underline.dataset.speechWordStart))
                    && Number(underline.dataset.speechWordLength) > 0;
                reader.setSpeechSession('');
                return result;
            })())JS")).toBool();
            QVERIFY(heuristicWordApplied);
        }
        QTRY_VERIFY(evaluate(start, QStringLiteral("selectionPopup.visible")).toBool());
        script(QStringLiteral(R"JS(
            globalThis.speechMarks = () => (reader?.view?.renderer?.getContents() ?? [])
                .flatMap(({overlayer}) => Array.from(overlayer?.element.querySelectorAll('[data-speech-segment]') ?? []));
        )JS"));
        if (!useMenu) {
            evaluate(start, QStringLiteral("selectionPopup.close()"));
            QTest::qWait(100);
            QVERIFY(start->property("enabled").toBool());
        }
    } else {
        const QString path = dir.filePath(QStringLiteral("speech.pdf"));
        {
            QPdfWriter writer(path);
            writer.setResolution(72);
            QPainter painter(&writer);
            painter.drawText(70, 100, QStringLiteral("Nur diese Auswahl."));
            painter.drawText(70, 200, QStringLiteral("Dieser Text darf nicht vorgelesen werden."));
        }
        QVERIFY(page->setProperty("filename", path));
        QVERIFY(page->setProperty("url", QUrl::fromLocalFile(path).toString()));
        auto *document = page->findChild<OkularPdfDocument *>();
        QVERIFY(document);
        QTRY_VERIFY(document->opened());
        // Select a region containing only the first line, then use the real menu.
        auto area = std::make_unique<Okular::RegularAreaRect>();
        area->append(Okular::NormalizedRect(0.05, 0.05, 0.95, 0.17));
        document->setTextSelection(0, std::move(area));
        evaluate(start, QStringLiteral("view.openSelectionMenu(100, 100)"));
        QTRY_VERIFY(start->property("enabled").toBool());
        auto *menu = page->findChild<QObject *>(QStringLiteral("pdfTextSelectionMenu"));
        QVERIFY(menu);
        if (!useMenu) {
            QVERIFY(QMetaObject::invokeMethod(menu, "close"));
            QTRY_VERIFY(!menu->property("visible").toBool());
            QTest::qWait(100);
            QVERIFY(start->property("enabled").toBool());
        }
    }
    if (paragraphs) {
        QCOMPARE(evaluate(start, QStringLiteral("root.speechSelection")).toList().size(), 13);
        const QString result = script(QStringLiteral(R"JS((() => {
            const doc = reader.view.renderer.getContents()[0].doc;
            globalThis.paragraphSourceDOM = new WeakMap([[doc, doc.body.innerHTML]]);
            const range = doc.createRange(); range.selectNodeContents(doc.getElementById('selection'));
            const selection = doc.getSelection();
            const previous = selection.rangeCount ? selection.getRangeAt(0).cloneRange() : null;
            selection.removeAllRanges(); selection.addRange(range);
            const selected = reader.collectSpeechSegments(range, 0, selection.toString());
            selection.removeAllRanges(); if (previous) selection.addRange(previous);
            const streamed = reader.collectSpeechSegments(range, 0);
            if (selected.length !== 13 || streamed.length !== 13) return 'wrong block/sentence count';
            const ids = ['first','first','first','first','first','first',
                'heading-one','heading-two','second','third','mixed','mixed','mixed'];
            for (let i = 0; i < selected.length; i++) {
                const segment = selected[i];
                const source = reader.view.resolveCFI(segment.blockId, reader.view.cfiFilter).anchor(doc);
                const block = source.startContainer;
                if (block?.id !== ids[i] || block.localName !== segment.blockTag) return 'wrong block CFI';
                if (streamed[i].blockId !== segment.blockId || streamed[i].text.trim() !== segment.text.trim())
                    return 'selection/stream identity differs';
            }
            if (new Set(selected.slice(0,6).map(s => s.blockId)).size !== 1) return 'inline markup split paragraph';
            if (new Set(selected.slice(10).map(s => s.blockId)).size !== 1) return 'language change lost paragraph';
            return doc.body.innerHTML === paragraphSourceDOM.get(doc) ? '' : 'DOM changed';
        })())JS")).toString();
        QVERIFY2(result.isEmpty(), qPrintable(result));
        const bool pendingMarkApplied = script(QStringLiteral(R"JS((() => {
            const segment = root.speechSelection[0];
            const mark = segment.marks[0];
            const session = 'pending-word-mark-test';
            reader.setSpeechSession(session);
            reader.showSpeechMark(session, segment.id, mark);
            reader.showSpeechSegment(session, segment);
            const content = reader.view.renderer.getContents().find(item => item.index === segment.sectionIndex);
            const underline = content.overlayer.element.querySelector('[data-speech-word-start]');
            const result = underline?.dataset.speechSegment === segment.id
                && underline.dataset.speechWordStart === String(mark.start)
                && underline.dataset.speechWordLength === String(mark.length);
            reader.setSpeechSession('');
            return result;
        })())JS")).toBool();
        QVERIFY(pendingMarkApplied);
    }
    QSignalSpy paragraphStarted(speech, &SpeechController::segmentStarted);
    QSignalSpy paragraphWords(speech, &SpeechController::indexMarkReached);
    if (sanskrit) script(QStringLiteral("globalThis.sanskritSourceDOM = new WeakMap(reader.view.renderer.getContents().map(c => [c.doc, c.doc.body.innerHTML]))"));
    if (continuous) script(QStringLiteral(R"JS((() => {
        const start = reader.startSpeechStream.bind(reader);
        reader.startSpeechStream = (...args) => {
            globalThis.restartSpeechCfi = args[1];
            globalThis.releaseSpeechStart = async () => {
                reader.startSpeechStream = start;
                await start(...args);
                globalThis.cancelledSpeechStartDone = true;
            };
        };
    })())JS"));
    if (!useMenu) {
        QVERIFY(QMetaObject::invokeMethod(controls, "open"));
        QVERIFY(QTest::qWaitForWindowExposed(controls));
    }
    auto *readButton = useMenu ? readMenuItem : startButton;
    QTRY_VERIFY(readButton->isVisible() && readButton->isEnabled());
    clickButton(readButton, continuous ? Qt::ControlModifier : Qt::NoModifier);
    if (continuous) {
        // Closing before the first batch cancels the stream even while the
        // controller is still Idle. Its delayed reply must not restart audio.
        QTRY_VERIFY(controls->isVisible());
        const int pendingStream = page->property("speechStreamId").toInt();
        controls->close();
        QTRY_VERIFY(!controls->isVisible());
        QVERIFY(!page->property("continuousSpeech").toBool());
        QVERIFY(page->property("speechStreamId").toInt() != pendingStream);
        QTRY_VERIFY(script(QStringLiteral("typeof releaseSpeechStart === 'function'")).toBool());
        script(QStringLiteral("releaseSpeechStart()"));
        QTRY_VERIFY(script(QStringLiteral("globalThis.cancelledSpeechStartDone === true")).toBool());
        QTest::qWait(100);
        QCOMPARE(speech->state(), SpeechController::Idle);
        QCOMPARE(server.messages().size(), 0);
        QVERIFY(!controls->isVisible());
        evaluate(start, QStringLiteral("root.readDocumentFrom(%1)").arg(encoded(script(QStringLiteral("restartSpeechCfi")).toString())));
    }
    QTRY_VERIFY_WITH_TIMEOUT(server.messages().size() == 1 || !speech->errorString().isEmpty(), 15000);
    QVERIFY2(server.messages().size() == 1, qPrintable(speech->errorString()));
    // Selection/menu activation automatically opens the native controls.
    QTRY_VERIFY(controls->isVisible());
    QVERIFY(QTest::qWaitForWindowExposed(controls));
    controls->setPosition(controls->position() + QPoint(35, 20));
    const QPoint movedPosition = controls->position();
    if (continuous) {
        QTRY_VERIFY(modeButton->isEnabled());
        QVERIFY(mode->property("checkable").toBool());
        QVERIFY(!mode->property("checked").toBool());
        QVERIFY(evaluate(mode, QStringLiteral("icon.source.toString()")).toString().endsWith(QStringLiteral("speech-continuous.svg")));
        QVERIFY(!speech->repeatSelection());
    } else {
        QVERIFY(modeButton->isEnabled());
        QCOMPARE(mode->property("checked").toBool(), repeat);
    }
    if (crossrefs) {
        const auto selection = page->property("speechSelection").toList();
        QCOMPARE(selection.size(), 8);
        const QStringList labels{QStringLiteral("भगवद्गीता, %1 2").arg(englishReferences ? QStringLiteral("Chapter") : QStringLiteral("Kapitel")),
            QStringLiteral("Śrīmad-Bhāgavatam, %1 1.2.3").arg(englishReferences ? QStringLiteral("Verse") : QStringLiteral("Vers")),
            QStringLiteral("Algebra II, %1 (4.7)").arg(englishReferences ? QStringLiteral("Equation") : QStringLiteral("Gleichung"))};
        const QStringList anchors{QStringLiteral("hier"), QStringLiteral("dort"), QStringLiteral("Formel")};
        int reference = 0;
        for (int i = 0; i < selection.size(); ++i) {
            const auto segment = selection[i].toMap();
            QCOMPARE(segment.value(QStringLiteral("lang")).toString(), QStringLiteral("de"));
            if (i < 7) QCOMPARE(segment.value(QStringLiteral("sentenceId")), selection[0].toMap().value(QStringLiteral("sentenceId")));
            else QVERIFY(segment.value(QStringLiteral("sentenceId")) != selection[0].toMap().value(QStringLiteral("sentenceId")));
            if (segment.value(QStringLiteral("type")).toString() != QLatin1String("reference")) continue;
            QCOMPARE(segment.value(QStringLiteral("text")).toString(), labels[reference]);
            const auto sourceMap = segment.value(QStringLiteral("sourceMap")).toList();
            QCOMPARE(sourceMap.size(), 1);
            QVERIFY(!sourceMap[0].toMap().value(QStringLiteral("exact")).toBool());
            const auto source = script(QStringLiteral("reader.view.resolveCFI(%1, reader.view.cfiFilter).anchor(reader.view.renderer.getContents()[0].doc).toString()")
                .arg(encoded(segment.value(QStringLiteral("cfi")).toString()))).toString();
            QCOMPARE(source, anchors[reference++]);
            for (const auto &value : segment.value(QStringLiteral("marks")).toList())
                QCOMPARE(value.toMap().value(QStringLiteral("cfi")), segment.value(QStringLiteral("cfi")));
        }
        QCOMPARE(reference, 3);
        const auto message = server.messages().first();
        QCOMPARE(message.language, englishReferences ? QStringLiteral("en") : QStringLiteral("de"));
        QCOMPARE(message.dataMode, QStringLiteral("SSML"));
        const QString expectedSpeech = englishReferences
            ? QStringLiteral("Siehe bhagavadgita, Chapter 2, Shreemad-Bhagavatam, Verse 1.2.3 und Algebra II, Equation (4.7). Zweiter Satz.")
            : QStringLiteral("Siehe bhagawadgita, Kapitel 2, Schrimad-Bhagawatam, Vers 1.2.3 und Algebra II, Gleichung (4.7). Zweiter Satz.");
        QCOMPARE(message.text.trimmed(), expectedSpeech + (continuous ? QStringLiteral("Normale Links und Notizen.U. Vers 9") : QString()));
        QCOMPARE(message.rawText.count(QStringLiteral("<s>")), continuous ? 4 : 2);
        QCOMPARE(message.rawText.count(QStringLiteral("<break")), continuous ? 3 : 1);
        QCOMPARE(bookReads, 1);
        QCOMPARE(referenceReads, 1);
        int words = 0;
        for (const auto &mark : message.marks) {
            server.sendIndexMark(message.id, message.client, mark.name);
            if (mark.name.startsWith(QStringLiteral("arianna-segment-"))) continue;
            ++words;
            QTRY_COMPARE(paragraphWords.size(), words);
            const auto reached = paragraphWords.last();
            const auto segmentId = reached.at(1).toString();
            const auto word = reached.at(2).toMap();
            const auto found = std::find_if(paragraphStarted.cbegin(), paragraphStarted.cend(), [&](const QList<QVariant> &value) {
                return value.at(1).toMap().value(QStringLiteral("id")).toString() == segmentId;
            });
            QVERIFY(found != paragraphStarted.cend());
            const auto segment = found->at(1).toMap();
            QTRY_COMPARE(script(QStringLiteral("speechMarks()[0]?.dataset.speechSegment")).toString(), segmentId);
            QTRY_COMPARE(script(QStringLiteral("speechMarks()[0]?.dataset.speechWordStart")).toInt(), word.value(QStringLiteral("start")).toInt());
            const QString expected = segment.value(QStringLiteral("type")).toString() == QLatin1String("reference")
                ? segment.value(QStringLiteral("text")).toString() == QLatin1String("U. Vers 9")
                    ? QStringLiteral("unbekannt") : anchors[labels.indexOf(segment.value(QStringLiteral("text")).toString())]
                : segment.value(QStringLiteral("text")).toString().mid(word.value(QStringLiteral("start")).toInt(), word.value(QStringLiteral("length")).toInt());
            QVERIFY(script(QStringLiteral(R"JS((() => {
                const {doc} = reader.view.renderer.getContents()[0];
                const source = reader.view.resolveCFI(%1, reader.view.cfiFilter).anchor(doc);
                const drawn = speechMarks()[0]?.querySelector('rect');
                return source.toString() === %2 && drawn && doc.body.innerHTML === referenceSourceDOM
                    && Math.abs(Number(drawn.getAttribute('width')) - source.getClientRects()[0].width) < 0.1;
            })())JS").arg(encoded(word.value(QStringLiteral("cfi")).toString()), encoded(expected))).toBool());
        }
        QCOMPARE(words, continuous ? 22 : 15);
        if (repeat || continuous) {
            server.finish(message.id);
            QTRY_COMPARE(server.messages().size(), 2);
            if (repeat) QCOMPARE(server.messages().last().text, message.text);
            else QVERIFY(server.messages().last().text.contains(QStringLiteral("Leading text.")));
        } else {
            server.finish(message.id);
            QTRY_COMPARE(speech->state(), SpeechController::Idle);
            QCOMPARE(server.messages().size(), 1);
        }
        QVERIFY(controls->close());
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
        if (continuous) {
            script(QStringLiteral(R"JS(
                globalThis.referenceReturnDone = false;
                reader.view.goTo(%1).then(() => reader.prepareSpeech(reader.view.renderer.getContents()[0].doc))
                    .then(() => referenceReturnDone = true);
            )JS").arg(encoded(selection[0].toMap().value(QStringLiteral("cfi")).toString())));
            QTRY_VERIFY(script(QStringLiteral("referenceReturnDone")).toBool());
        }
        const QString additional = script(QStringLiteral(R"JS((() => {
            const doc = reader.view.renderer.getContents()[0].doc;
            const collect = node => { const range = doc.createRange(); range.selectNodeContents(node); return reader.collectSpeechSegments(range, 0); };
            if (collect(doc.getElementById('missing'))[0]?.text !== 'U. Vers 9') return 'Missing-title fallback failed';
            const node = doc.querySelector('[data-ref="verse-ref"]').firstChild;
            const range = doc.createRange(); range.setStart(node, 1); range.setEnd(node, 2);
            const partial = reader.collectSpeechSegments(range, 0);
            if (partial.length !== 1 || partial[0].type !== 'reference' || !partial[0].text.includes('1.2.3')) return 'Partial anchor selection failed';
            const other = collect(doc.getElementById('other'));
            if (other.some(s => s.type === 'reference') || other.map(s => s.text).join('') !== 'Normale Links und Notizen.') return 'Ordinary link/annotation changed';
            if (collect(doc.body).some(s => s.text.includes('Geheim') || s.text.includes('H. Vers 9'))) return 'Hidden reference spoken';
            return doc.body.innerHTML === referenceSourceDOM ? '' : 'Reference DOM changed';
        })())JS")).toString();
        QVERIFY2(additional.isEmpty(), qPrintable(additional));
        // Editing library metadata must invalidate runtime aliases, never the EPUB.
        referenceBooks[0] = QJsonObject{{QStringLiteral("id"), QStringLiteral("gita")}, {QStringLiteral("title"), QStringLiteral("BHAGAVAD-GĪTĀ")}};
        script(QStringLiteral(R"JS(
            globalThis.referenceRefreshDone = false;
            reader.invalidateSpeechReferences();
            reader.prepareSpeech(reader.view.renderer.getContents()[0].doc).then(() => referenceRefreshDone = true);
        )JS"));
        QTRY_VERIFY(script(QStringLiteral("referenceRefreshDone")).toBool());
        QCOMPARE(bookReads, 2);
        QCOMPARE(referenceReads, 2);
        QVERIFY(script(QStringLiteral(R"JS((() => {
            const doc = reader.view.renderer.getContents()[0].doc;
            const range = doc.createRange(); range.selectNodeContents(doc.getElementById('chapter-ref'));
            return reader.collectSpeechSegments(range, 0)[0].text.startsWith('Bhagavad-Gītā, ')
                && doc.body.innerHTML === referenceSourceDOM;
        })())JS")).toBool());
        QFile original(dir.filePath(QStringLiteral("speech.epub")));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), epubBytes);
        return;
    }
    if (repeat) {
        const QString session = speech->sessionId();
        const auto selection = page->property(epub ? "speechSelection" : "speechSelectionText");
        if (epub) script(QStringLiteral("globalThis.repeatSourceDOM = reader.view.renderer.getContents()[0].doc.body.innerHTML"));
        for (int cycle = 0; cycle < 3; ++cycle) {
            QTRY_COMPARE(server.messages().size(), cycle + 1);
            const auto message = server.messages().last();
            QCOMPARE(message.text.trimmed(), QStringLiteral("Nur diese Auswahl."));
            QCOMPARE(speech->sessionId(), session);
            QTRY_COMPARE(paragraphStarted.size(), cycle + 1);
            if (epub) {
                const auto segment = paragraphStarted.last().at(1).toMap();
                const auto words = segment.value(QStringLiteral("marks")).toList();
                QCOMPARE(message.marks.size(), words.size());
                if (cycle) QVERIFY(message.marks.first().name != server.messages().first().marks.first().name);
                for (qsizetype j = 0; j < words.size(); ++j) {
                    const auto word = words.at(j).toMap();
                    server.sendIndexMark(message.id, message.client, message.marks.at(j).name);
                    QTRY_COMPARE(script(QStringLiteral("speechMarks()[0]?.dataset.speechWordStart")).toInt(), word.value(QStringLiteral("start")).toInt());
                    QVERIFY(script(QStringLiteral(R"JS((() => {
                        const {doc} = reader.view.renderer.getContents()[0];
                        const range = reader.view.resolveCFI(%1, reader.view.cfiFilter).anchor(doc);
                        const rect = range.getClientRects()[0];
                        const drawn = speechMarks()[0].querySelector('rect');
                        return range.toString() === %2 && doc.body.innerHTML === repeatSourceDOM
                            && Math.abs(Number(drawn.getAttribute('width')) - rect.width) < 0.1;
                    })())JS").arg(encoded(word.value(QStringLiteral("cfi")).toString()),
                        encoded(segment.value(QStringLiteral("text")).toString().mid(word.value(QStringLiteral("start")).toInt(), word.value(QStringLiteral("length")).toInt())))).toBool());
                }
            }
            if (cycle == 1) {
                // Updating the live selection must not change what is repeated.
                page->setProperty(epub ? "speechSelection" : "speechSelectionText", epub ? QVariant(QVariantList{}) : QVariant(QString()));
                QVERIFY(!startButton->isEnabled());
                clickButton(pauseButton, Qt::NoModifier);
                QTRY_COMPARE(speech->state(), SpeechController::Paused);
                QVERIFY(startButton->isEnabled());
                server.finish(message.id);
                QTest::qWait(100);
                QCOMPARE(server.messages().size(), 2);
                clickButton(startButton, Qt::NoModifier);
            } else {
                if (cycle == 2) {
                    clickMode();
                    clickMode();
                }
                server.finish(message.id);
            }
        }
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        QVERIFY(!speech->repeatSelection());
        QVERIFY(!mode->property("checked").toBool());
        QCOMPARE(server.messages().size(), 3);
        if (epub) QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
        page->setProperty(epub ? "speechSelection" : "speechSelectionText", selection);
        QVERIFY(startButton->isEnabled());
        clickMode();
        QMetaObject::invokeMethod(start, "trigger");
        QTRY_COMPARE(server.messages().size(), 4);
        const auto stopped = server.messages().last();
        clickButton(stopButton, Qt::NoModifier);
        server.sendOldEnd(stopped.id, stopped.client);
        if (!stopped.marks.isEmpty()) server.sendIndexMark(stopped.id, stopped.client, stopped.marks.first().name);
        QTest::qWait(100);
        QCOMPARE(speech->state(), SpeechController::Idle);
        QCOMPARE(server.messages().size(), 4);
        QVERIFY(speech->repeatSelection());
        if (epub) QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
        QMetaObject::invokeMethod(start, "trigger");
        QTRY_COMPARE(server.messages().size(), 5);
        // Native Close must stop repeats, including their retained selection.
        const auto closingRepeat = server.messages().last();
        const QString repeatSession = speech->sessionId();
        QVERIFY(controls->close());
        QTRY_VERIFY(!controls->isVisible());
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        QVERIFY(speech->sessionId() != repeatSession);
        speech->resume();
        server.sendOldEnd(closingRepeat.id, closingRepeat.client);
        QTest::qWait(100);
        QCOMPARE(server.messages().size(), 5);
        QVERIFY(speech->repeatSelection());
        if (epub) QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
        QMetaObject::invokeMethod(start, "trigger");
        QTRY_COMPARE(server.messages().size(), 6);
        QTRY_VERIFY(controls->isVisible());
        QCOMPARE(controls->position(), movedPosition);
        clickButton(pauseButton);
        QTRY_COMPARE(speech->state(), SpeechController::Paused);
        const auto pausedRepeat = server.messages().last();
        QVERIFY(controls->close());
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        speech->resume();
        server.sendOldEnd(pausedRepeat.id, pausedRepeat.client);
        QTest::qWait(100);
        QCOMPARE(server.messages().size(), 6);
        page->setProperty("url", QString());
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        QTRY_VERIFY(!controls->isVisible());
        QTest::qWait(100);
        QCOMPARE(server.messages().size(), 6);
        return;
    }
    if (sanskrit) {
        const QStringList expected{QStringLiteral("Before"), QStringLiteral("धर्मः रक्षति।"), QStringLiteral("After."),
            QStringLiteral("सत्यं वद॥"), QStringLiteral("शान्तिः।"),
            QStringLiteral("Weiter"), QStringLiteral("संस्कृतम्।"), QStringLiteral("Fertig.")};
        const QStringList languages{QStringLiteral("en"), QStringLiteral("sa"), QStringLiteral("en"),
            QStringLiteral("sa"), QStringLiteral("sa"), QStringLiteral("de"), QStringLiteral("sa"), QStringLiteral("de")};
        int messageCount = 0;
        const int count = continuous ? 8 : 5;
        for (int i = 0; i < count; ++i) {
            ++messageCount;
            QTRY_COMPARE(server.messages().size(), messageCount);
            auto message = server.messages().last();
            QCOMPARE(message.text.trimmed(), expected.at(i));
            QCOMPARE(message.language, languages.at(i));
            QCOMPARE(message.module, languages.at(i) == QLatin1String("sa") ? QStringLiteral("vits") : QStringLiteral("piper"));
            QTRY_VERIFY(!paragraphStarted.isEmpty());
            QTRY_COMPARE(paragraphStarted.last().at(1).toMap().value(QStringLiteral("text")).toString().trimmed(), expected.at(i));
            const auto segment = paragraphStarted.last().at(1).toMap();
            QCOMPARE(segment.value(QStringLiteral("href")).toString(), i < 5 ? QStringLiteral("chapter.xhtml") : QStringLiteral("next.xhtml"));
            if (i == 1) {
                speech->pause();
                QTRY_COMPARE(speech->state(), SpeechController::Paused);
                QVERIFY(server.commands().contains(QStringLiteral("CANCEL SELF")));
                server.sendOldEnd(message.id, message.client);
                speech->resume();
                ++messageCount;
                QTRY_COMPARE(server.messages().size(), messageCount);
                message = server.messages().last();
                QCOMPARE(message.text.trimmed(), expected.at(i));
            }
            const auto words = segment.value(QStringLiteral("marks")).toList();
            QCOMPARE(message.marks.size(), words.size());
            for (qsizetype j = 0; j < words.size(); ++j) {
                const auto word = words.at(j).toMap();
                server.sendIndexMark(message.id, message.client, message.marks.at(j).name);
                const QString check = QStringLiteral(R"JS((() => {
                    const content = reader.view.renderer.getContents().find(item => item.index === %1);
                    if (!content) return false;
                    const {doc, overlayer} = content;
                    if (!sanskritSourceDOM.has(doc)) sanskritSourceDOM.set(doc, doc.body.innerHTML);
                    const range = reader.view.resolveCFI(%2, reader.view.cfiFilter).anchor(doc);
                    const mark = overlayer.element.querySelector('[data-speech-word-start]');
                    const rect = range.getClientRects()[0];
                    const drawn = mark?.querySelector('rect');
                    return range.toString() === %3 && mark?.dataset.speechWordStart === %4
                        && mark.dataset.speechSegment === %5
                        && drawn && Math.abs(Number(drawn.getAttribute('width')) - rect.width) < 0.1
                        && doc.body.innerHTML === sanskritSourceDOM.get(doc);
                })())JS").arg(QString::number(segment.value(QStringLiteral("sectionIndex")).toInt()),
                    encoded(word.value(QStringLiteral("cfi")).toString()),
                    encoded(segment.value(QStringLiteral("text")).toString().mid(word.value(QStringLiteral("start")).toInt(),
                                                                                word.value(QStringLiteral("length")).toInt())),
                    encoded(word.value(QStringLiteral("start")).toString()), encoded(segment.value(QStringLiteral("id")).toString()));
                QTRY_VERIFY(script(check).toBool());
            }
            server.finish(message.id);
        }
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        QCOMPARE(server.messages().size(), messageCount);
        QVERIFY(speech->errorString().isEmpty());
        page->setProperty("url", QString());
        return;
    }
    if (paragraphs) {
        QStringList expected{QStringLiteral("Erster Satz. Zweiter Satz. Dritter Satz. Vierter Satz. Fünfter Satz. Sechster Satz."),
            QStringLiteral("Erste Überschrift"), QStringLiteral("Zweite Überschrift"),
            QStringLiteral("Ein weiterer Absatz ohne PunktEin neuer Absatz ohne PunktGuten Tag."),
            QStringLiteral("Hello world."), QStringLiteral("Deutsch zurück.")};
        if (continuous) expected.append(QStringLiteral("Weiter geht es. ").repeated(26).trimmed());
        qsizetype words = 0;
        for (qsizetype i = 0; i < expected.size(); ++i) {
            QTRY_COMPARE_WITH_TIMEOUT(server.messages().size(), i + 1, 15000);
            const auto message = server.messages().last();
            QCOMPARE(message.text.simplified(), expected.at(i));
            QCOMPARE(message.language, i == 4 ? QStringLiteral("en") : QStringLiteral("de"));
            if (i == 3) {
                QCOMPARE(packageTexts(message.rawText).size(), 3);
                QCOMPARE(message.rawText.count(QStringLiteral("<s>")), 3);
                QCOMPARE(message.rawText.count(QStringLiteral("<break time=\"400ms\"/>")), 2);
            }
            if (continuous && i == 5) {
                // The next chapter must not appear while this one still speaks.
                QTest::qWait(100);
                QCOMPARE(script(QStringLiteral("reader.view.renderer.getContents()[0].index")).toInt(), 0);
            }
            if (i == 0) {
                QCOMPARE(message.rawText.count(QStringLiteral("<s>")), 6);
                QCOMPARE(message.rawText.count(QStringLiteral("<break time=\"200ms\"/>")), 5);
            }
            for (const auto &mark : message.marks) {
                server.sendIndexMark(message.id, message.client, mark.name);
                if (mark.name.startsWith(QLatin1String("arianna-segment-"))) continue;
                ++words;
                QTRY_COMPARE(paragraphWords.size(), words);
                const auto word = paragraphWords.last().at(2).toMap();
                const auto segment = paragraphStarted.last().at(1).toMap();
                const QString correct = script(QStringLiteral(R"JS((() => {
                    const mark = %1;
                    const {index, anchor} = reader.view.resolveCFI(mark.cfi, reader.view.cfiFilter);
                    const {doc, overlayer} = reader.view.renderer.getContents().find(c => c.index === index);
                    if (!paragraphSourceDOM.has(doc)) paragraphSourceDOM.set(doc, doc.body.innerHTML);
                    const drawn = overlayer.element.querySelector('[data-speech-segment]');
                    const rects = Array.from(drawn?.querySelectorAll('rect') ?? []);
                    const range = anchor(doc);
                    return JSON.stringify({word:range.toString(), correct:
                        Number(drawn?.dataset.speechWordStart) === mark.start && rects.length > 0
                        && rects.every(rect => Number(rect.getAttribute('height')) === 2)
                        && doc.body.innerHTML === paragraphSourceDOM.get(doc)});
                })())JS").arg(QString::fromUtf8(QJsonDocument::fromVariant(word).toJson(QJsonDocument::Compact)))).toString();
                const auto shown = QJsonDocument::fromJson(correct.toUtf8()).object();
                QVERIFY2(shown.value(QStringLiteral("correct")).toBool(), qPrintable(correct));
                QCOMPARE(shown.value(QStringLiteral("word")).toString(), segment.value(QStringLiteral("text")).toString().mid(
                    word.value(QStringLiteral("start")).toInt(), word.value(QStringLiteral("length")).toInt()));
            }
            server.finish(message.id);
        }
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        QCOMPARE(paragraphStarted.size(), continuous ? 39 : 13);
        QCOMPARE(server.messages().size(), expected.size());
        QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
        if (continuous) {
            QVERIFY(!page->property("continuousSpeech").toBool());
            QCOMPARE(script(QStringLiteral("reader.view.renderer.getContents()[0].index")).toInt(), 3);
        }
        return;
    }
    if (mathPackets) {
        const QString inlineText = complexMath ? QStringLiteral("Vor zum Sprechen zu komplizierte Formel nach.")
            : QStringLiteral("Vor x zum Quadrat plus eins nach.");
        const QStringList expected = continuous
            ? QStringList{inlineText, QStringLiteral("a divided by b"),
                QStringLiteral("Danach fertig."), QStringLiteral("Next formula.")}
            : QStringList{inlineText};
        QSignalSpy segmentStarted(speech, &SpeechController::segmentStarted);
        QSignalSpy wordReached(speech, &SpeechController::indexMarkReached);
        for (qsizetype i = 0; i < expected.size(); ++i) {
            QTRY_COMPARE_WITH_TIMEOUT(server.messages().size(), i + 1, 15000);
            const auto message = server.messages().last();
            QCOMPARE(message.text.trimmed(), expected[i]);
            if (i == 0) {
                QCOMPARE(message.rawText.count(QStringLiteral("<s>")), 1);
                QVERIFY(!message.rawText.contains(QStringLiteral("<break")));
                const auto parts = evaluate(start, QStringLiteral("root.speechSelection")).toList();
                if (!continuous) QCOMPARE(parts.size(), 3);
                qsizetype words = 0;
                for (const auto &mark : message.marks) {
                    const qsizetype count = segmentStarted.size();
                    server.sendIndexMark(message.id, message.client, mark.name);
                    if (mark.name.startsWith(QLatin1String("arianna-segment-"))) {
                        if (mark.name == message.marks.first().name) continue;
                        QTRY_VERIFY(segmentStarted.size() > count);
                        const auto segment = segmentStarted.last().at(1).toMap();
                        if (segment.value(QStringLiteral("type")).toString() != QLatin1String("math")) continue;
                        QTRY_COMPARE(script(QStringLiteral("speechMarks()[0]?.dataset.speechType")).toString(), QStringLiteral("math"));
                        const QString correct = script(QStringLiteral(R"JS((() => {
                            const {doc} = reader.view.renderer.getContents()[0];
                            const svg = doc.getElementById('inline').querySelector('svg');
                            const bounds = svg.getBoundingClientRect();
                            const rects = speechMarks()[0].querySelectorAll('rect');
                            return rects.length === 1 && Number(rects[0].getAttribute('height')) === 2
                                && Math.abs(Number(rects[0].getAttribute('width')) - bounds.width) < 0.1
                                && mathSpeechMarkup() === mathSourceDOM ? '' : 'Incorrect inline formula underline';
                        })())JS")).toString();
                        QVERIFY2(correct.isEmpty(), qPrintable(correct));
                    } else {
                        ++words;
                        QTRY_COMPARE(wordReached.size(), words);
                        const auto word = wordReached.last().at(2).toMap();
                        QTRY_COMPARE(script(QStringLiteral("speechMarks()[0]?.dataset.speechWordStart")).toString(), QString::number(word.value(QStringLiteral("start")).toInt()));
                    }
                }
                QTRY_COMPARE(wordReached.size(), 2);
                QCOMPARE(wordReached.last().at(2).toMap().value(QStringLiteral("start")).toInt(), 1);
            } else if (i == 1 || i == 3) {
                QCOMPARE(message.dataMode, QStringLiteral("TEXT"));
                QTRY_COMPARE(script(QStringLiteral("speechMarks()[0]?.dataset.speechType")).toString(), QStringLiteral("math"));
            }
            server.finish(message.id);
        }
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
        if (continuous) QCOMPARE(script(QStringLiteral("reader.view.renderer.getContents()[0].index")).toInt(), 3);
        page->setProperty("url", QString());
        return;
    }
    if (math) {
        QCOMPARE(server.messages().last().text, QStringLiteral("x zum Quadrat plus eins"));
        const QStringList expected = continuous
            ? QStringList{QStringLiteral("x zum Quadrat plus eins"), QStringLiteral("nach."), QStringLiteral("a divided by b"), QStringLiteral("Danach fertig."), QStringLiteral("Next formula.")}
            : QStringList{QStringLiteral("x zum Quadrat plus eins")};
        const QString session = speech->sessionId();
        for (qsizetype i = 0; i < expected.size(); ++i) {
            QTRY_COMPARE_WITH_TIMEOUT(server.messages().size(), i + 1, 15000);
            QCOMPARE(speech->sessionId(), session);
            const auto message = server.messages().last();
            QCOMPARE(message.text.trimmed(), expected[i]);
            QCOMPARE(message.language, i == 2 || i == 4 ? QStringLiteral("en") : QStringLiteral("de"));
            if (i == 0 || i == 2 || i == 4) {
                QVERIFY(message.marks.isEmpty());
                QCOMPARE(message.dataMode, QStringLiteral("TEXT"));
                QTRY_COMPARE(script(QStringLiteral("speechMarks()[0]?.dataset.speechType")).toString(), QStringLiteral("math"));
                const QString id = i == 0 ? QStringLiteral("inline") : i == 2 ? QStringLiteral("display") : QStringLiteral("final");
                if (i == 4) script(QStringLiteral("mathSourceDOM = mathSpeechMarkup()"));
                const auto geometry = [&] { return script(QStringLiteral(R"JS((() => {
                    const {doc, overlayer} = reader.view.renderer.getContents()[0];
                    const svg = doc.getElementById(%1).querySelector('svg');
                    const expected = svg.getBoundingClientRect();
                    const rects = speechMarks()[0].querySelectorAll('rect');
                    return JSON.stringify({correct: rects.length === 1 && Math.abs(Number(rects[0].getAttribute('x')) - expected.left) < 0.1
                        && Math.abs(Number(rects[0].getAttribute('y')) - (expected.bottom - 2)) < 0.1
                        && Math.abs(Number(rects[0].getAttribute('width')) - expected.width) < 0.1
                        && mathSpeechMarkup() === mathSourceDOM,
                        expected: {x:expected.left, y:expected.bottom-2, width:expected.width},
                        actual: Array.from(rects, r => ({x:r.getAttribute('x'), y:r.getAttribute('y'), width:r.getAttribute('width')})),
                        unchanged: mathSpeechMarkup() === mathSourceDOM,
                        source: mathSpeechMarkup() === mathSourceDOM ? undefined : mathSourceDOM,
                        current: mathSpeechMarkup() === mathSourceDOM ? undefined : mathSpeechMarkup()});
                })())JS").arg(encoded(id))).toString(); };
                QTRY_VERIFY2(geometry().contains(QStringLiteral("\"correct\":true")), qPrintable(geometry()));
                if (i == 0) {
                    // Redraw after MathJax replaces its SVG children. The CFI
                    // and underline stay on the whole container.
                    QVERIFY(script(QStringLiteral(R"JS((() => {
                        const {doc, overlayer} = reader.view.renderer.getContents()[0];
                        const svg = doc.getElementById('inline').querySelector('svg');
                        svg.replaceWith(svg.cloneNode(true)); overlayer.redraw();
                        return speechMarks()[0].querySelectorAll('rect').length === 1 && mathSpeechMarkup() === mathSourceDOM;
                    })())JS")).toBool());
                    speech->pause();
                    QTRY_COMPARE(speech->state(), SpeechController::Paused);
                    QCOMPARE(script(QStringLiteral("speechMarks()[0]?.dataset.speechType")).toString(), QStringLiteral("math"));
                    speech->resume();
                    QTRY_COMPARE(speech->state(), SpeechController::Speaking);
                }
            }
            server.finish(message.id);
        }
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
        QVERIFY(!page->property("continuousSpeech").toBool());
        if (continuous) {
            QCOMPARE(script(QStringLiteral("reader.view.renderer.getContents()[0].index")).toInt(), 3);
        } else {
            // Emulate the EPUB's async MathJax-script arriving after selection,
            // followed by startup and a delayed accessibility-worker result.
            script(QStringLiteral(R"JS((() => {
                const doc = reader.view.renderer.getContents()[0].doc;
                const formula = doc.getElementById('inline');
                formula.removeAttribute('data-semantic-speech');
                formula.removeAttribute('data-semantic-speech-none');
                const loader = doc.createElement('script'); loader.id = 'MathJax-script';
                loader.type = 'application/json'; loader.setAttribute('src', 'https://cdn.jsdelivr.net/npm/mathjax@4/tex-svg.js'); doc.head.append(loader);
                doc.defaultView.MathJax = {};
                setTimeout(() => {
                    doc.defaultView.MathJax = {
                        startup: {promise: new Promise(resolve => setTimeout(resolve, 150))},
                        whenReady: action => new Promise(resolve => setTimeout(() => {
                            formula.setAttribute('data-semantic-speech', 'Bereit zum Vorlesen');
                            action(); resolve();
                        }, 150)),
                    };
                }, 150);
                const range = doc.createRange(); range.selectNode(formula);
                formula.dispatchEvent(new PointerEvent('pointerdown', {bubbles:true}));
                doc.getSelection().removeAllRanges(); doc.getSelection().addRange(range);
                formula.dispatchEvent(new PointerEvent('pointerup', {bubbles:true}));
            })())JS"));
            QTest::qWait(100);
            QVERIFY(!start->property("enabled").toBool());
            QTRY_VERIFY(start->property("enabled").toBool());
            QTRY_VERIFY(evaluate(start, QStringLiteral("selectionPopup.visible")).toBool());
            clickButton(readMenuItem, Qt::NoModifier);
            QTRY_COMPARE(server.messages().size(), 2);
            QCOMPARE(server.messages().last().text, QStringLiteral("Bereit zum Vorlesen"));
            QTRY_COMPARE(script(QStringLiteral("speechMarks()[0]?.dataset.speechType")).toString(), QStringLiteral("math"));
            QMetaObject::invokeMethod(stop, "trigger");
            QTRY_COMPARE(speech->state(), SpeechController::Idle);
            QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
            // An unavailable MathJax loader must not stall a prose selection.
            script(QStringLiteral(R"JS((() => {
                const doc = reader.view.renderer.getContents()[0].doc;
                doc.defaultView.MathJax = {startup:{promise:new Promise(() => {})}};
                const p = doc.body.querySelector(':scope > p:last-of-type');
                const range = doc.createRange(); range.selectNodeContents(p);
                p.dispatchEvent(new PointerEvent('pointerdown', {bubbles:true}));
                doc.getSelection().removeAllRanges(); doc.getSelection().addRange(range);
                p.dispatchEvent(new PointerEvent('pointerup', {bubbles:true}));
            })())JS"));
            QTRY_VERIFY_WITH_TIMEOUT(evaluate(start, QStringLiteral("selectionPopup.visible")).toBool(), 2000);
            clickButton(readMenuItem, Qt::NoModifier);
            QTRY_COMPARE_WITH_TIMEOUT(server.messages().size(), 3, 2000);
            QCOMPARE(server.messages().last().text.trimmed(), QStringLiteral("Danach fertig."));
            QMetaObject::invokeMethod(stop, "trigger");
            QTRY_COMPARE(speech->state(), SpeechController::Idle);
        }
        page->setProperty("url", QString());
        return;
    }
    QCOMPARE(server.messages().last().text.trimmed(), QStringLiteral("Nur diese Auswahl."));
    if (modeToggle) {
        const auto first = server.messages().last();
        const QString session = speech->sessionId();
        const QString source = script(QStringLiteral("reader.view.renderer.getContents()[0].doc.body.innerHTML")).toString();
        clickMode(); // Once -> repeat.
        clickMode(); // Repeat -> continuous, extending the captured selection.
        QTRY_VERIFY(page->property("continuousSpeech").toBool());
        QCOMPARE(page->property("speechMode").toInt(), 2);
        QTest::qWait(150);
        QCOMPARE(server.messages().size(), 1);
        QCOMPARE(speech->sessionId(), session);
        QVERIFY(!server.commands().contains(QStringLiteral("CANCEL SELF")));
        server.finish(first.id);
        QTRY_COMPARE(server.messages().size(), 2);
        const auto buffered = server.messages().last();
        QVERIFY(buffered.text.startsWith(QStringLiteral("Dieser Text darf nicht vorgelesen werden.")));
        QTRY_COMPARE(paragraphStarted.size(), 2);
        const auto current = paragraphStarted.last().at(1).toMap();
        QCOMPARE(current.value(QStringLiteral("text")).toString().trimmed(), QStringLiteral("Dieser Text darf nicht vorgelesen werden."));
        QVERIFY(script(QStringLiteral(R"JS((() => {
            const {doc} = reader.view.renderer.getContents()[0];
            const range = reader.view.resolveCFI(%1, reader.view.cfiFilter).anchor(doc);
            return range.toString().trim();
        })())JS").arg(encoded(current.value(QStringLiteral("cfi")).toString()))).toString() == current.value(QStringLiteral("text")).toString().trimmed());
        clickMode(); // Continuous -> finish current sentence once.
        QVERIFY(!page->property("continuousSpeech").toBool());
        const auto nextBoundary = std::find_if(buffered.marks.cbegin() + 1, buffered.marks.cend(), [](const auto &mark) {
            return mark.name.startsWith(QLatin1String("arianna-segment-"));
        });
        QVERIFY(nextBoundary != buffered.marks.cend());
        server.sendIndexMark(buffered.id, buffered.client, nextBoundary->name);
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        if (sentenceRepeat) {
            // It ended between the two clicks: repeat still captures this
            // sentence, while explicit Stop/Close clears that opportunity.
            clickMode();
            QTRY_COMPARE(server.messages().size(), 3);
            QCOMPARE(server.messages().last().text.trimmed(), current.value(QStringLiteral("text")).toString().trimmed());
            server.finish(server.messages().last().id);
            QTRY_COMPARE(server.messages().size(), 4);
            QCOMPARE(server.messages().last().text.trimmed(), current.value(QStringLiteral("text")).toString().trimmed());
            clickButton(stopButton);
        }
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        QCOMPARE(server.messages().size(), sentenceRepeat ? 4 : 2);
        QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
        QCOMPARE(script(QStringLiteral("reader.view.renderer.getContents()[0].doc.body.innerHTML")).toString(), source);
        if (!sentenceRepeat) {
            // Two clicks before the worker reports Speaking must still promote
            // the asynchronously restarted sentence to document reading.
            QMetaObject::invokeMethod(mode, "trigger");
            QMetaObject::invokeMethod(mode, "trigger");
            QTRY_VERIFY(page->property("continuousSpeech").toBool());
            QTRY_COMPARE(server.messages().size(), 3);
            QCOMPARE(server.messages().last().text.trimmed(), current.value(QStringLiteral("text")).toString().trimmed());
            server.finish(server.messages().last().id);
            QTRY_COMPARE(server.messages().size(), 4);
            QVERIFY(server.messages().last().text.startsWith(QStringLiteral("Davor Deutsch")));
            clickButton(stopButton);
            QTRY_COMPARE(speech->state(), SpeechController::Idle);
        }
        // A fresh Start in continuous mode still starts at the selection's
        // beginning. Closing cancels the queued document and keeps the mode.
        while (page->property("speechMode").toInt() != 2) clickMode();
        const int beforeStart = server.messages().size();
        QMetaObject::invokeMethod(start, "trigger");
        QTRY_COMPARE(server.messages().size(), beforeStart + 1);
        QVERIFY(server.messages().last().text.startsWith(QStringLiteral("Nur diese Auswahl.")));
        QVERIFY(controls->close());
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        QVERIFY(!page->property("continuousSpeech").toBool());
        QCOMPARE(page->property("speechMode").toInt(), 2);
        page->setProperty("url", QString());
        return;
    }
    if (continuous) {
        const QString session = speech->sessionId();
        QVERIFY(page->property("continuousSpeech").toBool());
        QTRY_VERIFY(!evaluate(start, QStringLiteral("selectionPopup.visible")).toBool());
        speech->pause();
        QTRY_COMPARE(speech->state(), SpeechController::Paused);
        server.finish(server.messages().last().id);
        QTest::qWait(100);
        QCOMPARE(server.messages().size(), 1);
        speech->resume();
        const int total = 30;
        for (int i = 1; i < total; ++i) {
            QTRY_COMPARE_WITH_TIMEOUT(server.messages().size(), i + 1, 15000);
            const auto message = server.messages().last();
            QCOMPARE(speech->sessionId(), session);
            QVERIFY(!message.text.contains(QStringLiteral("vor der Auswahl")));
            QVERIFY(!message.text.contains(QStringLiteral("Unsichtbarer")));
            QVERIFY(!message.text.contains(QStringLiteral("Keinesfalls")));
            if (epub && i == 2) {
                QVERIFY(message.marks.size() > 300);
                const auto last = message.marks.last();
                const double previousPage = script(QStringLiteral("reader.view.renderer.start")).toDouble();
                server.sendIndexMark(message.id, message.client, last.name);
                QTRY_COMPARE(script(QStringLiteral("speechMarks()[0]?.dataset.speechWordStart")).toString(), QString::number(last.start));
                const auto followPosition = script(QStringLiteral(R"JS((() => {
                    const {doc} = reader.view.renderer.getContents()[0];
                    const range = doc.createRange();
                    range.selectNodeContents(doc.body.lastElementChild);
                    const words = reader.collectSpeechSegments(range, 0)[0].marks;
                    const word = reader.view.resolveCFI(words.at(-1).cfi, reader.view.cfiFilter).anchor(doc);
                    const rect = word.getBoundingClientRect();
                    const frame = doc.defaultView.frameElement.getBoundingClientRect();
                    const viewport = reader.view.renderer.getBoundingClientRect();
                    return JSON.stringify({visible: rect.left + frame.left >= viewport.left - 1
                        && rect.right + frame.left <= viewport.right + 1
                        && rect.top + frame.top >= viewport.top - 1
                        && rect.bottom + frame.top <= viewport.bottom + 1,
                        word: word.toString(), left: rect.left, frame: frame.left,
                        viewport: viewport.left, size: viewport.width});
                })())JS")).toString();
                QVERIFY2(followPosition.contains(QStringLiteral("\"visible\":true")), qPrintable(followPosition));
                QVERIFY(script(QStringLiteral("reader.view.renderer.start")).toDouble() > previousPage);
            }
            if (epub && i >= 3) {
                QCOMPARE(message.language, QStringLiteral("en"));
                QCOMPARE(script(QStringLiteral("reader.view.renderer.getContents()[0].index")).toInt(), 3);
            }
            server.finish(message.id);
        }
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        QVERIFY(!page->property("continuousSpeech").toBool());
        QVERIFY(!speech->repeatSelection());
        QCOMPARE(page->property("speechMode").toInt(), 2);
        QVERIFY(modeButton->isEnabled());
        QVERIFY(!mode->property("checked").toBool());
        QVERIFY(evaluate(mode, QStringLiteral("icon.source.toString()")).toString().endsWith(QStringLiteral("speech-continuous.svg")));
        QCOMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
        // The keyboard alternative starts a stream. Closing while paused must
        // invalidate it before queued callbacks can restart playback or marks.
        script(QStringLiteral(R"JS((() => {
            const doc = reader.view.renderer.getContents()[0].doc;
            const p = doc.body.querySelector('p');
            const range = doc.createRange(); range.selectNodeContents(p);
            p.dispatchEvent(new PointerEvent('pointerdown', {bubbles: true}));
            doc.getSelection().removeAllRanges(); doc.getSelection().addRange(range);
            p.dispatchEvent(new PointerEvent('pointerup', {bubbles: true}));
        })())JS"));
        QTRY_VERIFY(evaluate(start, QStringLiteral("selectionPopup.opened")).toBool());
        readMenuItem->window()->requestActivate();
        readMenuItem->forceActiveFocus();
        QTRY_VERIFY(readMenuItem->hasActiveFocus());
        QTest::keyClick(readMenuItem->window(), Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(server.messages().size(), total + 1);
        QVERIFY(page->property("continuousSpeech").toBool());
        QTRY_VERIFY(!evaluate(start, QStringLiteral("selectionPopup.visible")).toBool());
        const auto cancelled = server.messages().last();
        clickButton(pauseButton, Qt::NoModifier);
        QTRY_COMPARE(speech->state(), SpeechController::Paused);
        QVERIFY(controls->close());
        QTRY_VERIFY(!controls->isVisible());
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        QVERIFY(!page->property("continuousSpeech").toBool());
        QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
        server.sendOldBegin(cancelled.id, cancelled.client);
        server.sendOldEnd(cancelled.id, cancelled.client);
        speech->resume();
        if (!cancelled.marks.isEmpty()) server.sendIndexMark(cancelled.id, cancelled.client, cancelled.marks.first().name);
        QTest::qWait(100);
        QCOMPARE(server.messages().size(), total + 1);
        QCOMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
        page->setProperty("url", QString());
        return;
    }
    if (epub) {
        QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 1);
        QVERIFY(script(QStringLiteral("speechMarks()[0].querySelectorAll('rect').length > 0")).toBool());
        QCOMPARE(script(QStringLiteral("Number(speechMarks()[0].querySelector('rect').getAttribute('height'))")).toInt(), 2);
        const auto message = server.messages().last();
        QCOMPARE(message.dataMode, QStringLiteral("SSML"));
        QCOMPARE(message.marks.size(), 3);
        // A real SSIP index event must traverse libspeechd, the worker, QML and
        // WebEngine before the overlay is narrowed to the second word.
        server.sendIndexMark(message.id, message.client, message.marks.at(1).name);
        QTRY_COMPARE(script(QStringLiteral("speechMarks()[0]?.dataset.speechWordStart")).toInt(), 4);
        const QString wordCfi = evaluate(start, QStringLiteral("root.speechSelection[0].marks[1].cfi")).toString();
        QVERIFY(script(QStringLiteral(R"JS((() => {
            const doc = reader.view.renderer.getContents()[0].doc;
            const range = reader.view.resolveCFI(%1, reader.view.cfiFilter).anchor(doc);
            const expected = range.getClientRects()[0];
            const drawn = speechMarks()[0].querySelector('rect');
            return range.toString() === 'diese' && Math.abs(Number(drawn.getAttribute('width')) - expected.width) < 0.1;
        })())JS").arg(encoded(wordCfi))).toBool());
    }
    if (useMenu) {
        if (epub)
            QTRY_VERIFY(!evaluate(start, QStringLiteral("selectionPopup.visible")).toBool());
        else
            QTRY_VERIFY(!page->findChild<QObject *>(QStringLiteral("pdfTextSelectionMenu"))->property("visible").toBool());
        QTest::qWait(100); // Let the menu finish closing before clicking the controls.
    }
    QTRY_VERIFY(pause->property("enabled").toBool());
    clickButton(pauseButton, Qt::NoModifier);
    QTRY_COMPARE(speech->state(), SpeechController::Paused);
    if (epub) {
        QCOMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 1);
        QCOMPARE(script(QStringLiteral("speechMarks()[0]?.dataset.speechWordStart")).toInt(), 4);
    }
    QVERIFY(controls->isVisible());
    clickButton(startButton, Qt::NoModifier);
    QTRY_COMPARE(speech->state(), SpeechController::Speaking);
    clickButton(stopButton, Qt::NoModifier);
    QTRY_COMPARE(speech->state(), SpeechController::Idle);
    if (epub)
        QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
    QTRY_VERIFY(server.commands().contains(QStringLiteral("CANCEL SELF")));
    QVERIFY(!stop->property("enabled").toBool());
    // Closing/replacing the book cancels an active session and invalidates its selection.
    QMetaObject::invokeMethod(start, "trigger");
    QTRY_COMPARE(server.messages().size(), 2);
    if (epub) {
        QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 1);
        // Replace the active selection with three language runs. The same source
        // map must survive normalization, inline markup and UTF-16 surrogate pairs.
        script(QStringLiteral(R"JS((() => {
            const doc = reader.view.renderer.getContents()[0].doc;
            const p = doc.getElementById('mixed');
            const range = doc.createRange();
            range.setStart(p.firstChild, 6);
            range.setEnd(p.lastChild, p.lastChild.length - 7);
            doc.dispatchEvent(new PointerEvent('pointerdown', {bubbles: true}));
            const selection = doc.getSelection();
            selection.removeAllRanges();
            selection.addRange(range);
            globalThis.expectedSpeechText = selection.toString();
            globalThis.testSpeechSegments = reader.collectSpeechSegments(range, 0, expectedSpeechText);
            globalThis.speechSourceDOM = doc.body.innerHTML;
            doc.dispatchEvent(new PointerEvent('pointerup', {bubbles: true}));
        })())JS"));
        QTRY_COMPARE(evaluate(start, QStringLiteral("root.speechSelection.length")).toInt(), 3);
        QCOMPARE(script(QStringLiteral("testSpeechSegments.map(s => s.lang).join(',')")).toString(), QStringLiteral("de,en,de"));
        QCOMPARE(script(QStringLiteral("testSpeechSegments.map(s => s.text).join('')")).toString(), script(QStringLiteral("expectedSpeechText")).toString());
        QCOMPARE(script(QStringLiteral(R"JS((() => {
            const doc = reader.view.renderer.getContents()[0].doc;
            let normalized = false;
            for (const segment of testSpeechSegments) {
                if (!segment.id || segment.sectionIndex !== 0 || segment.href !== 'chapter.xhtml') return 'missing identity';
                const source = reader.view.resolveCFI(segment.cfi, reader.view.cfiFilter).anchor(doc).toString();
                if (source.includes('Davor') || source.includes('danach')) return 'selection boundaries escaped';
                let offset = 0;
                for (const span of segment.sourceMap) {
                    if (span.spokenStart !== offset || span.spokenLength <= 0) return 'invalid UTF-16 span';
                    const spoken = segment.text.slice(offset, offset + span.spokenLength);
                    const original = reader.view.resolveCFI(span.cfi, reader.view.cfiFilter).anchor(doc).toString();
                    if (span.exact && spoken !== original) return 'source text mismatch';
                    normalized ||= !span.exact;
                    offset += span.spokenLength;
                }
                if (offset !== segment.text.length) return 'incomplete source map';
            }
            return normalized ? '' : 'normalization not exercised';
        })())JS")).toString(), QString());
        QTRY_VERIFY(evaluate(start, QStringLiteral("selectionPopup.visible")).toBool());
        evaluate(start, QStringLiteral("selectionPopup.close()"));
        QTRY_VERIFY(!evaluate(start, QStringLiteral("selectionPopup.visible")).toBool());
        QMetaObject::invokeMethod(start, "trigger");
        QTRY_COMPARE(server.messages().size(), 3);
        const QVariantList segments = evaluate(start, QStringLiteral("root.speechSelection")).toList();
        QCOMPARE(segments.size(), 3);
        for (int i = 0; i < 3; ++i) {
            const auto segment = segments.at(i).toMap();
            QTRY_COMPARE(server.messages().size(), 3 + i);
            QCOMPARE(server.messages().last().text, segment.value(QStringLiteral("text")).toString()
                .replace(QStringLiteral("Śaṅkarācārya"), QStringLiteral("Schankaratscharja"))
                .replace(QStringLiteral("Śrī Kṛṣṇa"), QStringLiteral("Shree Krishna")));
            QCOMPARE(server.messages().last().language, i == 1 ? QStringLiteral("en") : QStringLiteral("de"));
            QTRY_COMPARE(script(QStringLiteral("speechMarks()[0]?.dataset.speechSegment")).toString(), segment.value(QStringLiteral("id")).toString());
            const auto message = server.messages().last();
            const auto words = segment.value(QStringLiteral("marks")).toList();
            QCOMPARE(message.marks.size(), words.size());
            for (qsizetype wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
                const auto word = words.at(wordIndex).toMap();
                server.sendIndexMark(message.id, message.client, message.marks.at(wordIndex).name);
                QTRY_COMPARE(script(QStringLiteral("speechMarks()[0]?.dataset.speechWordStart")).toString(), word.value(QStringLiteral("start")).toString());
                QVERIFY(script(QStringLiteral(R"JS((() => {
                    const doc = reader.view.renderer.getContents()[0].doc;
                    const range = reader.view.resolveCFI(%1, reader.view.cfiFilter).anchor(doc);
                    const original = %2;
                    return range.toString() === original && doc.body.innerHTML === speechSourceDOM;
                })())JS").arg(encoded(word.value(QStringLiteral("cfi")).toString()),
                              encoded(segment.value(QStringLiteral("text")).toString().mid(word.value(QStringLiteral("start")).toLongLong(),
                                                                                       word.value(QStringLiteral("length")).toLongLong())))).toBool());
            }
            if (i == 0) {
                // A personal annotation under the speech underline remains clickable,
                // and renderer redraws retain the underline without editing the text.
                QVERIFY(script(QStringLiteral(R"JS((() => {
                    const {doc, overlayer} = reader.view.renderer.getContents()[0];
                    const range = reader.view.resolveCFI(testSpeechSegments[0].cfi, reader.view.cfiFilter).anchor(doc);
                    overlayer.add('test-note', range, () => document.createElementNS('http://www.w3.org/2000/svg', 'g'));
                    // Re-add the speech marker on top of the personal annotation.
                    reader.showSpeechSegment(%1, testSpeechSegments[0]);
                    overlayer.redraw();
                    const rect = range.getClientRects()[0];
                    const hit = overlayer.hitTest({x: rect.x + rect.width / 2, y: rect.y + rect.height / 2});
                    return hit[0] === 'test-note' && speechMarks().length === 1 && doc.body.innerHTML === speechSourceDOM;
                })())JS").arg(encoded(speech->sessionId()))).toBool());
                // Restore the actual menu/toolbar snapshot ID, not the independently
                // collected test fixture ID, before checking asynchronous events.
                evaluate(start, QStringLiteral("view.runJavaScript('reader.showSpeechSegment(' + JSON.stringify(speech.sessionId) + ', ' + JSON.stringify(root.speechSelection[0]) + ')')"));
                QTRY_COMPARE(script(QStringLiteral("speechMarks()[0]?.dataset.speechSegment")).toString(), segment.value(QStringLiteral("id")).toString());
                speech->pause();
                QTRY_COMPARE(speech->state(), SpeechController::Paused);
                server.finish(server.messages().last().id);
                QTest::qWait(100);
                QCOMPARE(server.messages().size(), 3);
                QCOMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 1);
                speech->resume();
            } else {
                server.finish(server.messages().last().id);
            }
        }
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
        const auto old = server.messages().last();
        server.sendOldBegin(old.id, old.client);
        server.sendOldEnd(old.id, old.client);
        if (!old.marks.isEmpty()) server.sendIndexMark(old.id, old.client, old.marks.first().name);
        QTest::qWait(100);
        QCOMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
        // A whole paragraph in one language must advance sentence by sentence,
        // including boundaries inside a text node and sentences spanning markup.
        script(QStringLiteral(R"JS((() => {
            const doc = reader.view.renderer.getContents()[0].doc;
            const p = doc.getElementById('sentences');
            const range = doc.createRange();
            range.selectNodeContents(p);
            p.dispatchEvent(new PointerEvent('pointerdown', {bubbles: true}));
            const selection = doc.getSelection();
            selection.removeAllRanges();
            selection.addRange(range);
            globalThis.expectedSentenceText = selection.toString();
            globalThis.sentenceSourceDOM = doc.body.innerHTML;
            p.dispatchEvent(new PointerEvent('pointerup', {bubbles: true}));
        })())JS"));
        QTRY_VERIFY(evaluate(start, QStringLiteral("selectionPopup.visible")).toBool());
        QTRY_COMPARE(evaluate(start, QStringLiteral("root.speechSelection.map(s => s.text).join('')")).toString(),
                     script(QStringLiteral("expectedSentenceText")).toString());
        const QVariantList sentenceSegments = evaluate(start, QStringLiteral("root.speechSelection")).toList();
        QCOMPARE(sentenceSegments.size(), 3);
        const QStringList expectedSentences{
            QStringLiteral("Erster Satz 😀."), QStringLiteral("Zweiter Satz kostet 3.14 Euro!"), QStringLiteral("„Dritter Satz é?“")};
        // Check all clipped UTF-16 mappings and the exact ranges that will be
        // underlined, not just the text sent to the speech backend.
        QCOMPARE(script(QStringLiteral(R"JS((() => {
            const doc = reader.view.renderer.getContents()[0].doc;
            const range = doc.createRange();
            range.selectNodeContents(doc.getElementById('sentences'));
            globalThis.sentenceSegments = reader.collectSpeechSegments(range, 0, expectedSentenceText);
            for (const segment of sentenceSegments) {
                const source = reader.view.resolveCFI(segment.cfi, reader.view.cfiFilter).anchor(doc).toString();
                if (source.replace(/\s+/gu, ' ').trim() !== segment.text.trim()) return 'sentence CFI escaped';
                let offset = 0;
                for (const span of segment.sourceMap) {
                    if (span.spokenStart !== offset || span.spokenLength <= 0) return 'invalid sentence offset';
                    const original = reader.view.resolveCFI(span.cfi, reader.view.cfiFilter).anchor(doc).toString();
                    if (span.exact && original !== segment.text.slice(offset, offset + span.spokenLength)) return 'clipped text mismatch';
                    offset += span.spokenLength;
                }
                if (offset !== segment.text.length) return 'incomplete sentence mapping';
                for (const mark of segment.marks) {
                    const original = reader.view.resolveCFI(mark.cfi, reader.view.cfiFilter).anchor(doc).toString();
                    if (original !== segment.text.slice(mark.start, mark.start + mark.length)) return 'word CFI mismatch';
                }
            }
            range.selectNodeContents(doc.getElementById('danda'));
            const sanskrit = reader.collectSpeechSegments(range, 0, range.toString());
            if (sanskrit.length !== 2 || sanskrit.some(s => s.lang !== 'sa')) return 'Sanskrit sentence boundaries';
            return '';
        })())JS")).toString(), QString());
        evaluate(start, QStringLiteral("selectionPopup.close()"));
        QTRY_VERIFY(!evaluate(start, QStringLiteral("selectionPopup.visible")).toBool());
        QMetaObject::invokeMethod(start, "trigger");
        for (int i = 0; i < 3; ++i) {
            const auto segment = sentenceSegments.at(i).toMap();
            QCOMPARE(segment.value(QStringLiteral("text")).toString().trimmed(), expectedSentences.at(i));
            QTRY_COMPARE(server.messages().size(), 6 + i);
            QCOMPARE(server.messages().last().text, segment.value(QStringLiteral("text")).toString());
            QCOMPARE(server.messages().last().language, QStringLiteral("de"));
            QTRY_COMPARE(script(QStringLiteral("speechMarks()[0]?.dataset.speechSegment")).toString(), segment.value(QStringLiteral("id")).toString());
            QVERIFY(script(QStringLiteral(R"JS((() => {
                const doc = reader.view.renderer.getContents()[0].doc;
                const range = reader.view.resolveCFI(sentenceSegments[%1].cfi, reader.view.cfiFilter).anchor(doc);
                const expected = Array.from(range.getClientRects());
                const drawn = Array.from(speechMarks()[0].querySelectorAll('rect'));
                return doc.body.innerHTML === sentenceSourceDOM && drawn.length === expected.length
                    && drawn.every((rect, i) => Math.abs(Number(rect.getAttribute('x')) - expected[i].left) < 0.1
                        && Math.abs(Number(rect.getAttribute('y')) - (expected[i].bottom - 2)) < 0.1
                        && Math.abs(Number(rect.getAttribute('width')) - expected[i].width) < 0.1);
            })())JS").arg(i)).toBool());
            const auto message = server.messages().last();
            const auto words = segment.value(QStringLiteral("marks")).toList();
            QCOMPARE(message.marks.size(), words.size());
            for (qsizetype wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
                const auto word = words.at(wordIndex).toMap();
                QCOMPARE(message.marks.at(wordIndex).start, word.value(QStringLiteral("start")).toLongLong());
                server.sendIndexMark(message.id, message.client, message.marks.at(wordIndex).name);
                QTRY_COMPARE(script(QStringLiteral("speechMarks()[0]?.dataset.speechWordStart")).toString(), word.value(QStringLiteral("start")).toString());
                QVERIFY(script(QStringLiteral(R"JS((() => {
                    const doc = reader.view.renderer.getContents()[0].doc;
                    const range = reader.view.resolveCFI(%1, reader.view.cfiFilter).anchor(doc);
                    const expected = Array.from(range.getClientRects());
                    const drawn = Array.from(speechMarks()[0].querySelectorAll('rect'));
                    return doc.body.innerHTML === sentenceSourceDOM && drawn.length === expected.length
                        && drawn.every((rect, i) => Math.abs(Number(rect.getAttribute('x')) - expected[i].left) < 0.1
                            && Math.abs(Number(rect.getAttribute('width')) - expected[i].width) < 0.1);
                })())JS").arg(encoded(word.value(QStringLiteral("cfi")).toString()))).toBool());
            }
            server.finish(server.messages().last().id);
        }
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
        QMetaObject::invokeMethod(start, "trigger");
        QTRY_COMPARE(server.messages().size(), 9);
        QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 1);
    }
    if (epub && !useMenu && !continuous && !math && !paragraphs && !sanskrit && !repeat) {
        speech->stop();
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        script(QStringLiteral(R"JS((() => {
            const configure = globalThis.setMathSpeechOptions;
            globalThis.mathSpeechOptionsSeen = [];
            globalThis.setMathSpeechOptions = options => {
                mathSpeechOptionsSeen.push(options.locale); configure(options);
            };
        })())JS"));
        for (const QString &locale : {QStringLiteral("de"), QStringLiteral("en")}) {
            const auto voices = speech->voices();
            const auto voice = std::find_if(voices.cbegin(), voices.cend(), [&](const QVariant &value) {
                return value.toMap().value(QStringLiteral("language")).toString() == locale;
            });
            QVERIFY(voice != voices.cend());
            Config::setSpeechVoice(voice->toMap().value(QStringLiteral("id")).toString());
            QTRY_COMPARE(page->property("mathSpeechLocale").toString(), locale);
            QTRY_COMPARE(script(QStringLiteral("mathSpeechOptionsSeen.at(-1)")).toString(), locale);
        }
        Config::setSpeechVoice(QString());
        QTRY_COMPARE(page->property("mathSpeechLocale").toString(), QString());
        QTRY_COMPARE(script(QStringLiteral("mathSpeechOptionsSeen.at(-1)")).toString(), QString());
        auto *formulaWindow = page->findChild<QObject *>(QStringLiteral("epubMathJaxWindow"));
        QVERIFY(formulaWindow);
        const QString description = QStringLiteral("1 plus 1 geteilt durch 2 Quadrat ist gleich pi Quadrat geteilt durch 6");
        const int previousMessages = server.messages().size();
        const QVariant segment = QVariantMap{{QStringLiteral("type"), QStringLiteral("math")},
            {QStringLiteral("text"), description}, {QStringLiteral("lang"), QStringLiteral("de")}};
        QVERIFY(QMetaObject::invokeMethod(formulaWindow, "readAloudRequested", Q_ARG(QVariant, segment)));
        QTRY_COMPARE(server.messages().size(), previousMessages + 1);
        QCOMPARE(server.messages().last().text, description);
        QCOMPARE(server.messages().last().language, QStringLiteral("de"));
        QVERIFY(controls->isVisible());
        QVERIFY(!page->property("continuousSpeech").toBool());
        server.finish(server.messages().last().id);
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
    }
    // A real window close clears a multi-segment queue, both during playback
    // and while paused. Resume and delayed callbacks must not revive it.
    const QVariantList closeQueue = epub ? page->property("speechSelection").toList() : QVariantList{
        QVariantMap{{QStringLiteral("id"), QStringLiteral("close-first")}, {QStringLiteral("text"), QStringLiteral("Erster Eintrag.")}, {QStringLiteral("lang"), QStringLiteral("de")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("close-next")}, {QStringLiteral("text"), QStringLiteral("Never read the queued entry.")}, {QStringLiteral("lang"), QStringLiteral("en")}}};
    QVERIFY(closeQueue.size() > 1);
    speech->stop();
    QTRY_COMPARE(speech->state(), SpeechController::Idle);
    for (bool paused : {false, true}) {
        QVERIFY(QMetaObject::invokeMethod(controls, "open"));
        QVERIFY(QTest::qWaitForWindowExposed(controls));
        QCOMPARE(controls->position(), movedPosition);
        const int previousMessages = server.messages().size();
        speech->speakSegments(closeQueue);
        QTRY_COMPARE(server.messages().size(), previousMessages + 1);
        const auto current = server.messages().last();
        const QString session = speech->sessionId();
        if (paused) {
            clickButton(pauseButton);
            QTRY_COMPARE(speech->state(), SpeechController::Paused);
        }
        const auto cancels = server.commands().count(QStringLiteral("CANCEL SELF"));
        QVERIFY(controls->close());
        QTRY_VERIFY(!controls->isVisible());
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        QTRY_VERIFY(server.commands().count(QStringLiteral("CANCEL SELF")) > cancels);
        QVERIFY(speech->sessionId() != session);
        QVERIFY(!stop->property("enabled").toBool());
        QVERIFY(!pause->property("enabled").toBool());
        speech->resume();
        speech->continueReading(session, closeQueue, true);
        server.sendOldBegin(current.id, current.client);
        server.sendOldEnd(current.id, current.client);
        if (!current.marks.isEmpty()) server.sendIndexMark(current.id, current.client, current.marks.first().name);
        QTest::qWait(100);
        QCOMPARE(speech->state(), SpeechController::Idle);
        QCOMPARE(server.messages().size(), previousMessages + 1);
        QVERIFY(!controls->isVisible());
        if (epub) QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
    }
    page->setProperty("url", QStringLiteral(""));
    QTRY_COMPARE(speech->state(), SpeechController::Idle);
    QVERIFY(!start->property("enabled").toBool());
    if (epub)
        QTRY_COMPARE(script(QStringLiteral("speechMarks().length")).toInt(), 0);
}

int main(int argc, char **argv)
{
    realSpeechAddress = qgetenv("SPEECHD_ADDRESS");
    // Resolve the live socket before isolating the test's XDG directories.
    if (realSpeechAddress.isEmpty() && !qEnvironmentVariableIsEmpty("XDG_RUNTIME_DIR"))
        realSpeechAddress = QByteArray("unix_socket:") + qgetenv("XDG_RUNTIME_DIR") + "/speech-dispatcher/speechd.sock";
    QTemporaryDir environment;
    const auto path = QFile::encodeName(environment.path());
    qputenv("XDG_DATA_HOME", path);
    qputenv("XDG_CONFIG_HOME", path);
    qputenv("XDG_CACHE_HOME", path);
    qputenv("XDG_RUNTIME_DIR", path);
    qputenv("LANGUAGE", "en");
    qputenv("SPEECHD_CMD", "/bin/false");
    QtWebEngineQuick::initialize();
    QGuiApplication application(argc, argv);
    KLocalizedString::setApplicationDomain("arianna");
    SpeechControllerTest test;
    return QTest::qExec(&test, argc, argv);
}
#endif
#include "speechcontrollertest.moc"
