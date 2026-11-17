// SPDX-License-Identifier: GPL-2.0-or-later

#include "speechcontroller.h"
#include "config.h"
#include "sanskritnumbers.h"
#include "transliterator.h"

#include <KLocalizedString>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStringList>
#include <QtMath>
#include <QTimer>
#include <QVector>
#include <QXmlStreamWriter>

#include <libspeechd.h>
#include <poll.h>

#include <cstdlib>
#include <utility>

namespace
{
QMutex callbackMutex;
QHash<size_t, SpeechWorker *> clients;

QString languageTag(QString language)
{
    return language.trimmed().replace(QLatin1Char('_'), QLatin1Char('-')).toLower();
}

QString germanCardinalBelowHundred(quint64 number)
{
    static const QStringList small{
        QStringLiteral("null"), QStringLiteral("eins"), QStringLiteral("zwei"), QStringLiteral("drei"),
        QStringLiteral("vier"), QStringLiteral("fünf"), QStringLiteral("sechs"), QStringLiteral("sieben"),
        QStringLiteral("acht"), QStringLiteral("neun"), QStringLiteral("zehn"), QStringLiteral("elf"),
        QStringLiteral("zwölf"), QStringLiteral("dreizehn"), QStringLiteral("vierzehn"), QStringLiteral("fünfzehn"),
        QStringLiteral("sechzehn"), QStringLiteral("siebzehn"), QStringLiteral("achtzehn"), QStringLiteral("neunzehn")};
    static const QStringList tens{
        QString(), QString(), QStringLiteral("zwanzig"), QStringLiteral("dreißig"), QStringLiteral("vierzig"),
        QStringLiteral("fünfzig"), QStringLiteral("sechzig"), QStringLiteral("siebzig"), QStringLiteral("achtzig"),
        QStringLiteral("neunzig")};
    if (number < 20) return small.at(number);
    const quint64 unit = number % 10;
    const QString prefix = unit == 1 ? QStringLiteral("ein") : small.at(unit);
    return unit ? prefix + QStringLiteral("und") + tens.at(number / 10) : tens.at(number / 10);
}

QString germanCardinalBelowThousand(quint64 number)
{
    const quint64 hundreds = number / 100;
    const quint64 remainder = number % 100;
    if (!hundreds) return germanCardinalBelowHundred(number);
    const QString prefix = (hundreds == 1 ? QStringLiteral("ein") : germanCardinalBelowHundred(hundreds))
        + QStringLiteral("hundert");
    return remainder ? prefix + germanCardinalBelowHundred(remainder) : prefix;
}

QString germanOrdinalBelowThousand(quint64 number)
{
    static const QStringList irregular{
        QString(), QStringLiteral("erste"), QStringLiteral("zweite"), QStringLiteral("dritte"),
        QStringLiteral("vierte"), QStringLiteral("fünfte"), QStringLiteral("sechste"), QStringLiteral("siebte"),
        QStringLiteral("achte"), QStringLiteral("neunte"), QStringLiteral("zehnte"), QStringLiteral("elfte"),
        QStringLiteral("zwölfte"), QStringLiteral("dreizehnte"), QStringLiteral("vierzehnte"),
        QStringLiteral("fünfzehnte"), QStringLiteral("sechzehnte"), QStringLiteral("siebzehnte"),
        QStringLiteral("achtzehnte"), QStringLiteral("neunzehnte")};
    if (number < irregular.size()) return irregular.at(number);
    if (number < 100) return germanCardinalBelowHundred(number) + QStringLiteral("ste");

    const quint64 hundreds = number / 100;
    const quint64 remainder = number % 100;
    const QString prefix = (hundreds == 1 ? QStringLiteral("ein") : germanCardinalBelowHundred(hundreds))
        + QStringLiteral("hundert");
    return remainder ? prefix + germanOrdinalBelowThousand(remainder) : prefix + QStringLiteral("ste");
}

QString germanOrdinal(quint64 number)
{
    if (number > 999999 || number == 0) return {};
    if (number < 1000) return germanOrdinalBelowThousand(number);
    const quint64 thousands = number / 1000;
    const quint64 remainder = number % 1000;
    const QString prefix = (thousands == 1 ? QStringLiteral("ein") : germanCardinalBelowThousand(thousands))
        + QStringLiteral("tausend");
    return remainder ? prefix + germanOrdinalBelowThousand(remainder) : prefix + QStringLiteral("ste");
}

bool germanOrdinalTakesEn(const QString &source, qsizetype start)
{
    const QString before = source.left(start);
    static const QRegularExpression weakEnding(
        QStringLiteral("(?:\\b(?:dem|den|des|einem|einen|eines|einer|diesem|diesen|dieses|jenem|jenen|jenes|am|im|zum|beim|vom|ins|ans)\\s+"
                       "|\\b(?:mit|nach|bei|von|zu|aus|außer|gegenüber|in|an|auf|unter|über|vor|hinter|neben|zwischen)\\s+"
                       "(?:der|dem|den)\\s+)$"),
        QRegularExpression::CaseInsensitiveOption);
    return weakEnding.match(before).hasMatch();
}

QString expandGermanAbbreviations(const QString &source, qsizetype start, qsizetype length)
{
    static const QRegularExpression abbreviations(
        QStringLiteral("(?<![\\p{L}\\p{M}\\p{N}])(?:a[\\s\\p{Zs}]*\\.[\\s\\p{Zs}]*c\\.(?=[\\s\\p{Zs}]+bhaktivedanta\\b)|[0-9]+\\.(?=\\s+[\\p{L}])|i\\s*\\.\\s*d\\s*\\.\\s*r\\.?|z\\s*\\.\\s*b\\.?|z\\s*\\.\\s*t\\.?|u\\s*\\.\\s*a\\.?|u\\s*\\.\\s*ä\\.?|bzw\\.?|usw\\.?|etc\\.?|d\\s*\\.\\s*h\\.?|ca\\.?|nr\\.?|dr\\.?)"
                       "(?![\\p{L}\\p{M}])"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression acInitials(QStringLiteral("^a[\\s\\p{Zs}]*\\.[\\s\\p{Zs}]*c\\.$"),
                                               QRegularExpression::CaseInsensitiveOption);
    static const QHash<QString, QString> expansions{
        {QStringLiteral("bzw"), QStringLiteral("beziehungsweise")},
        {QStringLiteral("ca"), QStringLiteral("circa")},
        {QStringLiteral("d.h"), QStringLiteral("das heißt")},
        {QStringLiteral("dr"), QStringLiteral("Doktor")},
        {QStringLiteral("etc"), QStringLiteral("et cetera")},
        {QStringLiteral("i.d.r"), QStringLiteral("in der Regel")},
        {QStringLiteral("nr"), QStringLiteral("Nummer")},
        {QStringLiteral("u.a"), QStringLiteral("unter anderem")},
        {QStringLiteral("u.ä"), QStringLiteral("und Ähnliches")},
        {QStringLiteral("usw"), QStringLiteral("und so weiter")},
        {QStringLiteral("z.b"), QStringLiteral("zum Beispiel")},
        {QStringLiteral("z.t"), QStringLiteral("zum Teil")},
    };

    start = qBound<qsizetype>(0, start, source.size());
    const qsizetype end = length < 0 ? source.size() : qBound(start, start + length, source.size());
    QString expanded;
    QVector<qsizetype> sourceToExpanded(source.size() + 1);
    qsizetype position = 0;
    auto matches = abbreviations.globalMatch(source);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const qsizetype matchStart = match.capturedStart();
        const qsizetype matchEnd = match.capturedEnd();
        while (position < matchStart) {
            sourceToExpanded[position] = expanded.size();
            expanded += source.at(position++);
        }
        sourceToExpanded[matchStart] = expanded.size();
        QString key = match.captured().toLower();
        QString replacement;
        const bool expandAcInitials = acInitials.match(match.captured()).hasMatch();
        static const QRegularExpression ordinalNumber(QStringLiteral("^([0-9]+)\\.$"));
        const auto ordinalMatch = ordinalNumber.match(key);
        if (expandAcInitials) {
            replacement = QStringLiteral("Ah Zeh");
        } else if (ordinalMatch.hasMatch()) {
            bool valid = false;
            const quint64 value = ordinalMatch.captured(1).toULongLong(&valid);
            replacement = valid ? germanOrdinal(value) : QString();
            if (!replacement.isEmpty() && germanOrdinalTakesEn(source, matchStart)) {
                replacement.chop(1);
                replacement += QStringLiteral("en");
            }
        } else {
            while (!key.isEmpty() && key.back().isPunct()) key.chop(1);
            key.replace(QRegularExpression(QStringLiteral("\\s+")), QString());
            replacement = expansions.value(key);
        }
        if (replacement.isEmpty()) {
            expanded += match.captured();
        } else {
            QString replacementWithCase = replacement;
            if (!match.captured().isEmpty() && match.captured().front().isUpper()) {
                replacementWithCase[0] = replacementWithCase.at(0).toUpper();
            }
            expanded += replacementWithCase;
        }
        if (expandAcInitials && !replacement.isEmpty()) {
            const qsizetype outputStart = sourceToExpanded.at(matchStart);
            const qsizetype cOffset = match.captured().toLower().indexOf(QLatin1Char('c'), 1);
            const qsizetype cStart = matchStart + cOffset;
            for (qsizetype boundary = matchStart + 1; boundary < matchEnd; ++boundary) {
                sourceToExpanded[boundary] = outputStart;
            }
            sourceToExpanded[matchStart + 1] = outputStart + 2;
            for (qsizetype boundary = matchStart + 2; boundary <= cStart; ++boundary) {
                sourceToExpanded[boundary] = outputStart + 3;
            }
            sourceToExpanded[cStart + 1] = outputStart + 6;
        } else {
            for (qsizetype boundary = matchStart + 1; boundary < matchEnd; ++boundary) {
                sourceToExpanded[boundary] = sourceToExpanded[matchStart];
            }
        }
        sourceToExpanded[matchEnd] = expanded.size();
        position = matchEnd;
    }
    while (position < source.size()) {
        sourceToExpanded[position] = expanded.size();
        expanded += source.at(position++);
    }
    sourceToExpanded[source.size()] = expanded.size();
    return expanded.mid(sourceToExpanded.at(start), sourceToExpanded.at(end) - sourceToExpanded.at(start));
}

QString speechLanguage(const QVariantMap &segment)
{
    const QString language = segment.value(QStringLiteral("lang")).toString();
    const QString base = languageTag(language).section(QLatin1Char('-'), 0, 0);
    static const QStringList devanagariLanguages{QStringLiteral("sa"), QStringLiteral("hi"), QStringLiteral("mr"),
        QStringLiteral("ne"), QStringLiteral("mai"), QStringLiteral("bho"), QStringLiteral("awa"), QStringLiteral("bra"),
        QStringLiteral("doi"), QStringLiteral("dgo"), QStringLiteral("kok"), QStringLiteral("new"), QStringLiteral("pi"),
        QStringLiteral("ks"), QStringLiteral("sd")};
    if (devanagariLanguages.contains(base)) return language;
    bool devanagari = false;
    for (const QChar character : segment.value(QStringLiteral("text")).toString()) {
        if (character.script() == QChar::Script_Devanagari) devanagari = true;
        else if (character.isLetter()) return language;
    }
    // The DOM reader splits mixed-script passages. This fallback also handles
    // a plain selection whose surrounding book metadata says English/German.
    return devanagari ? QStringLiteral("sa") : language;
}

// Pronunciation aliases belong to the backend payload, never to segment text
// or word/CFI offsets. Keep the actual selected voice's language authoritative.
QString pronounceIast(const QString &text, const QString &voiceLanguage)
{
    const QString language = languageTag(voiceLanguage).section(QLatin1Char('-'), 0, 0);
    if (language != QLatin1String("en") && language != QLatin1String("de")) return text;
    const bool german = language == QLatin1String("de");
    static const QRegularExpression words(QStringLiteral("[\\p{L}\\p{M}]+"));
    static const QString iastLetters = QStringLiteral("āīūṛṝḷḹṅñṭḍṇśṣṃṁḥ");
    QString result;
    qsizetype position = 0;
    auto matches = words.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const QString original = match.captured();
        const QString word = original.normalized(QString::NormalizationForm_C);
        bool iast = false;
        bool supported = true;
        for (const QChar character : word) {
            const QChar lower = character.toLower();
            if (iastLetters.contains(lower)) {
                // A lone Spanish ñ does not identify a Sanskrit transcription.
                iast |= lower != QChar(u'ñ');
            } else if (lower < QLatin1Char('a') || lower > QLatin1Char('z')) {
                supported = false;
                break;
            }
        }
        result += text.mid(position, match.capturedStart() - position);
        if (!iast || !supported) {
            result += original;
        } else {
            const bool upperCase = word == word.toUpper();
            for (const QChar character : word) {
                QString alias;
                switch (character.toLower().unicode()) {
                case u'ā': alias = QStringLiteral("a"); break;
                // English "Shri" gets /ai/, while "Geeta" loses Gita's hard g.
                case u'ī': alias = !german && word.startsWith(QStringLiteral("śrī"), Qt::CaseInsensitive)
                    ? QStringLiteral("ee") : QStringLiteral("i"); break;
                case u'ū': alias = german ? QStringLiteral("u") : QStringLiteral("oo"); break;
                case u'ṛ': case u'ṝ': alias = QStringLiteral("ri"); break;
                case u'ḷ': case u'ḹ': alias = QStringLiteral("li"); break;
                case u'ś': case u'ṣ': alias = german ? QStringLiteral("sch") : QStringLiteral("sh"); break;
                case u'ṅ': case u'ṇ': alias = QStringLiteral("n"); break;
                case u'ñ': alias = german ? QStringLiteral("nj") : QStringLiteral("ny"); break;
                case u'ṭ': alias = QStringLiteral("t"); break;
                case u'ḍ': alias = QStringLiteral("d"); break;
                case u'ṃ': case u'ṁ': alias = QStringLiteral("m"); break;
                case u'ḥ': alias = QStringLiteral("h"); break;
                case u'c': alias = german ? QStringLiteral("tsch") : QStringLiteral("ch"); break;
                case u'j': if (german) alias = QStringLiteral("dsch"); break;
                case u'y': if (german) alias = QStringLiteral("j"); break;
                case u'v': if (german) alias = QStringLiteral("w"); break;
                default: break;
                }
                if (alias.isEmpty()) {
                    result += character;
                } else {
                    if (upperCase) alias = alias.toUpper();
                    else if (character.isUpper()) alias[0] = alias.at(0).toUpper();
                    result += alias;
                }
            }
        }
        position = match.capturedEnd();
    }
    result += text.mid(position);
    return result;
}

QString pronounceSpeechText(QString text, const QString &voiceLanguage, const QVariantMap &segment)
{
    if (segment.value(QStringLiteral("type")).toString() == QLatin1String("reference")) {
        const QString language = languageTag(voiceLanguage).section(QLatin1Char('-'), 0, 0);
        if (language == QLatin1String("de") || language == QLatin1String("en")) {
            if (const auto transcription = Transliterator::devanagariToIast(text)) text = *transcription;
        } else if (language == QLatin1String("sa")) {
            // Transcribe Sanskrit titles, including their undiacritized words,
            // while retaining an ordinary foreign-language title as supplied.
            static const QRegularExpression iast(QStringLiteral("[āīūṛṝḷḹṅṭḍṇśṣṃṁḥ]"), QRegularExpression::CaseInsensitiveOption);
            static const QRegularExpression latinWords(QStringLiteral("[\\p{Latin}\\p{M}]+"));
            if (iast.match(segment.value(QStringLiteral("text")).toString()).hasMatch()) {
                QString converted;
                qsizetype position = 0;
                auto words = latinWords.globalMatch(text);
                while (words.hasNext()) {
                    const auto word = words.next();
                    converted += text.mid(position, word.capturedStart() - position);
                    converted += Transliterator::iastToDevanagari(word.captured()).value_or(word.captured());
                    position = word.capturedEnd();
                }
                text = converted + text.mid(position);
            }
        }
    }
    return pronounceIast(text, voiceLanguage);
}
}

// All synchronous libspeechd calls run here, never in the GUI thread.
class SpeechWorker : public QObject
{
    Q_OBJECT

public:
    explicit SpeechWorker(std::shared_ptr<std::atomic<quint64>> generation)
        : m_generation(std::move(generation))
    {
        auto *timer = new QTimer(this);
        timer->setInterval(1000);
        connect(timer, &QTimer::timeout, this, [this] {
            if (m_connection && disconnected()) {
                fail(i18n("The connection to Speech Dispatcher was lost."));
            }
        });
        // Start when the worker's thread starts its event loop.
        QMetaObject::invokeMethod(timer, qOverload<>(&QTimer::start), Qt::QueuedConnection);
    }

    void shutdown()
    {
        cancel();
        close();
    }

    void play(quint64 generation, const QVariantList &segments, const QString &voice, int rate, int sentencePauseMs,
              int paragraphPauseMs, bool expandSanskritNumbers, bool hasMore, bool selection)
    {
        cancel();
        m_session = generation;
        if (generation != *m_generation) {
            return;
        }
        m_preferredVoice = voice;
        m_expandSanskritNumbers = expandSanskritNumbers;
        m_rate = rate;
        setSentencePauseMs(sentencePauseMs);
        setParagraphPauseMs(paragraphPauseMs);
        if (!open() || !loadVoices()) {
            return;
        }
        m_segments = segments;
        // Retain the original selection, including word CFIs. Stream batches
        // never become repeatable, even when the selection mode is cyclic.
        m_selectionSegments = selection ? segments : QVariantList{};
        m_hasMore = hasMore;
        next();
    }

    void setRepeatSelection(bool repeat)
    {
        m_repeatSelection = repeat;
    }

    void extendReading(quint64 generation, const QString &requestId)
    {
        if (generation != m_session || generation != *m_generation) return;
        const auto last = !m_segments.isEmpty() ? m_segments.last().toMap()
            : !m_batch.isEmpty() ? m_batch.last().toMap()
            : !m_sentenceSegments.isEmpty() ? m_sentenceSegments.last().toMap() : m_currentSegment;
        if (last.isEmpty()) return;
        m_finishAfterSegment = -1;
        m_selectionSegments.clear();
        m_hasMore = true;
        m_waiting = true;
        m_preloadEmpty = false;
        Q_EMIT readingExtended(m_session, requestId, last);
    }

    static bool sameSentence(const QVariantMap &a, const QVariantMap &b)
    {
        const auto id = a.value(QStringLiteral("sentenceId")).toString();
        if (id.isEmpty()) return a.value(QStringLiteral("id")) == b.value(QStringLiteral("id"));
        return id == b.value(QStringLiteral("sentenceId")).toString()
            && a.value(QStringLiteral("blockId")) == b.value(QStringLiteral("blockId"))
            && a.value(QStringLiteral("href")) == b.value(QStringLiteral("href"));
    }

    void rememberSentence(const QVariantMap &segment)
    {
        if (!m_sentenceSegments.isEmpty() && !sameSentence(m_sentenceSegments.last().toMap(), segment))
            m_sentenceSegments.clear();
        if (m_sentenceSegments.isEmpty() || m_sentenceSegments.last().toMap().value(QStringLiteral("id")) != segment.value(QStringLiteral("id")))
            m_sentenceSegments.append(segment);
    }

    void finishSentence(quint64 generation, bool repeat)
    {
        if (generation != m_session || generation != *m_generation) return;
        m_repeatSelection = repeat;
        m_hasMore = false;
        m_waiting = false;
        m_preloadEmpty = false;
        QVariantList remaining;
        m_finishAfterSegment = m_message >= 0 ? m_segmentIndex : -1;
        for (qsizetype index = m_segmentIndex + 1; index < m_batch.size(); ++index) {
            if (!sameSentence(m_currentSegment, m_batch.at(index).toMap())) break;
            m_finishAfterSegment = index;
            remaining.append(m_batch.at(index));
        }
        QVariantList queued;
        // Only continue queued pieces of this sentence if the whole submitted
        // batch belongs to it. Later paragraphs and preloaded replies are dropped.
        if (m_batch.isEmpty() || m_finishAfterSegment == m_batch.size() - 1) {
            for (const auto &segment : std::as_const(m_segments)) {
                if (!sameSentence(m_currentSegment, segment.toMap())) break;
                queued.append(segment);
            }
        }
        m_selectionSegments = m_sentenceSegments + remaining;
        for (const auto &segment : std::as_const(queued)) {
            if (m_selectionSegments.isEmpty() || m_selectionSegments.last().toMap().value(QStringLiteral("id")) != segment.toMap().value(QStringLiteral("id")))
                m_selectionSegments.append(segment);
        }
        m_segments = queued;
        if (m_message < 0 && !m_paused) next();
    }

    void append(quint64 generation, const QVariantList &segments, bool hasMore)
    {
        if (generation != m_session || generation != *m_generation || !m_waiting) return;
        m_waiting = false;
        m_hasMore = hasMore;
        m_segments.append(segments);
        m_preloadEmpty = segments.isEmpty();
        if (!m_paused && m_message < 0) next();
    }

    void cancel()
    {
        const bool active = m_message >= 0;
        m_message = -1;
        m_segments.clear();
        m_selectionSegments.clear();
        m_sentenceSegments.clear();
        m_currentSegment.clear();
        m_currentMarks.clear();
        m_batch.clear();
        m_packageStarts.clear();
        m_segmentIndex = 0;
        m_segmentStarted = false;
        m_restartOnPause = false;
        m_paused = false;
        m_hasMore = false;
        m_waiting = false;
        m_preloadEmpty = false;
        m_finishAfterSegment = -1;
        if (active && m_connection && !disconnected()) {
            spd_cancel(m_connection);
        }
    }

    void pause(quint64 generation)
    {
        if (generation != m_session || generation != *m_generation || (m_message < 0 && !m_waiting) || m_paused) {
            return;
        }
        // Restart the current sentence for Piper batches and non-resumable
        // VITS output, retaining later sentences but not the completed batch.
        if ((m_batch.size() > 1 || m_restartOnPause) && m_message >= 0) {
            const QVariantList remaining = m_finishAfterSegment < 0 ? m_batch.mid(m_segmentIndex)
                : m_batch.mid(m_segmentIndex, m_finishAfterSegment - m_segmentIndex + 1);
            if (disconnected() || spd_cancel(m_connection) != 0) {
                fail(i18n("Speech Dispatcher could not pause the speech."));
                return;
            }
            m_message = -1;
            m_segments = remaining + m_segments;
            m_batch.clear();
            m_currentMarks.clear();
            m_finishAfterSegment = -1;
        } else if (disconnected() || (m_message >= 0 && spd_pause(m_connection) != 0)) {
            fail(i18n("Speech Dispatcher could not pause the speech."));
            return;
        }
        m_paused = true;
        Q_EMIT state(generation, SpeechController::Paused);
    }

    void resume(quint64 generation)
    {
        if (generation != m_session || generation != *m_generation || !m_paused) {
            return;
        }
        // Speech Dispatcher pauses the connection, so resume it even if the
        // current utterance ended while our queue was paused.
        if (disconnected() || spd_resume(m_connection) != 0) {
            fail(i18n("Speech Dispatcher could not resume the speech."));
            return;
        }
        m_paused = false;
        Q_EMIT state(generation, SpeechController::Speaking);
        if (m_message < 0) {
            next();
        }
    }

    void setRate(quint64 generation, int rate)
    {
        if (generation != m_session || generation != *m_generation || !m_connection) {
            return;
        }
        m_rate = rate;
        if (m_message < 0) return;
        if (disconnected() || spd_set_voice_rate(m_connection, rate) != 0) {
            fail(i18n("Speech Dispatcher could not change the speech rate."));
        }
    }

    void setSentencePauseMs(int milliseconds)
    {
        m_sentencePauseMs = qBound(0, milliseconds, 500);
    }

    void setParagraphPauseMs(int milliseconds)
    {
        m_paragraphPauseMs = qBound(0, milliseconds, 500);
    }

    void refresh(quint64 generation)
    {
        m_session = generation;
        if (open()) {
            loadVoices();
        }
    }

Q_SIGNALS:
    void state(quint64 generation, SpeechController::State state);
    void failed(quint64 generation, const QString &message);
    void voicesLoaded(const QVariantList &voices);
    void segmentStarted(quint64 generation, const QVariantMap &segment);
    void segmentFinished(quint64 generation, const QString &segmentId);
    void indexMarkReached(quint64 generation, const QString &segmentId, const QVariantMap &mark);
    void moreSegmentsRequested(quint64 generation, bool preload);
    void readingExtended(quint64 generation, const QString &requestId, const QVariantMap &lastSegment);

private:
    static void notification(size_t message, size_t client, SPDNotificationType event)
    {
        // The callback is made by libspeechd's receiver thread. Holding the lock
        // until invokeMethod returns protects the QObject against teardown.
        QMutexLocker lock(&callbackMutex);
        if (auto *worker = clients.value(client)) {
            QMetaObject::invokeMethod(
                worker,
                [worker, message, event] {
                    worker->handleNotification(message, event);
                },
                Qt::QueuedConnection);
        }
    }

    static void indexNotification(size_t message, size_t client, SPDNotificationType event, char *indexMark)
    {
        // libspeechd frees the name after the callback returns. Copy it before
        // handing the event to the worker's event loop.
        const QString name = QString::fromUtf8(indexMark ? indexMark : "");
        QMutexLocker lock(&callbackMutex);
        if (auto *worker = clients.value(client)) {
            QMetaObject::invokeMethod(worker, [worker, message, event, name] {
                if (event == SPD_EVENT_INDEX_MARK) worker->handleIndexMark(message, name);
            }, Qt::QueuedConnection);
        }
    }

    void handleNotification(size_t message, SPDNotificationType event)
    {
        if (m_session != *m_generation || m_message < 0 || message != size_t(m_message)) {
            return;
        }
        if (event == SPD_EVENT_BEGIN) {
            if (!m_segmentStarted) {
                m_segmentStarted = true;
                Q_EMIT segmentStarted(m_session, m_currentSegment);
            }
        } else if (event == SPD_EVENT_END) {
            m_message = -1;
            Q_EMIT segmentFinished(m_session, m_currentSegment.value(QStringLiteral("id")).toString());
            m_currentSegment.clear();
            m_currentMarks.clear();
            m_batch.clear();
            m_segmentStarted = false;
            m_finishAfterSegment = -1;
            if (!m_paused || (m_segments.isEmpty() && !m_hasMore)) {
                next();
            }
        } else if (event == SPD_EVENT_CANCEL) {
            m_message = -1;
            m_segments.clear();
            m_selectionSegments.clear();
            m_sentenceSegments.clear();
            m_currentSegment.clear();
            m_currentMarks.clear();
            m_batch.clear();
            m_segmentStarted = false;
            m_paused = false;
            m_hasMore = false;
            m_waiting = false;
            m_finishAfterSegment = -1;
            Q_EMIT state(m_session, SpeechController::Idle);
        }
    }

    void activateSegment(qsizetype index)
    {
        if (index < 0 || index >= m_batch.size()) return;
        if (index != m_segmentIndex) {
            Q_EMIT segmentFinished(m_session, m_currentSegment.value(QStringLiteral("id")).toString());
            m_segmentIndex = index;
            m_currentSegment = m_batch.at(index).toMap();
            rememberSentence(m_currentSegment);
            m_segmentStarted = false;
        }
        if (!m_segmentStarted) {
            m_segmentStarted = true;
            Q_EMIT segmentStarted(m_session, m_currentSegment);
        }
    }

    void handleIndexMark(size_t message, const QString &name)
    {
        if (m_session != *m_generation || m_message < 0 || message != size_t(m_message)) return;
        const auto mark = m_currentMarks.constFind(name);
        if (mark == m_currentMarks.cend() || mark->segment < m_segmentIndex) return;
        if (m_finishAfterSegment >= 0 && mark->segment > m_finishAfterSegment) {
            // Piper has already submitted later sentences in this utterance.
            // Cancel at their first boundary mark before starting their highlight.
            m_message = -1;
            if (disconnected() || spd_cancel(m_connection) != 0) {
                fail(i18n("Speech Dispatcher could not stop the speech."));
                return;
            }
            Q_EMIT segmentFinished(m_session, m_currentSegment.value(QStringLiteral("id")).toString());
            m_currentSegment.clear();
            m_currentMarks.clear();
            m_batch.clear();
            m_segmentStarted = false;
            m_finishAfterSegment = -1;
            if (!m_paused) next();
            return;
        }
        activateSegment(mark->segment);
        if (!mark->word.isEmpty()) {
            Q_EMIT indexMarkReached(m_session, m_currentSegment.value(QStringLiteral("id")).toString(), mark->word);
        }
    }

    bool disconnected() const
    {
        if (!m_connection) {
            return true;
        }
        const int socket = spd_fd(m_connection);
        if (socket < 0) {
            return true;
        }
        pollfd descriptor{socket, POLLRDHUP, 0};
        return ::poll(&descriptor, 1, 0) > 0 && (descriptor.revents & (POLLHUP | POLLRDHUP | POLLERR | POLLNVAL));
    }

    bool open()
    {
        if (m_connection && !disconnected()) {
            return true;
        }
        close();
        char *error = nullptr;
        m_connection = spd_open2("arianna", "selection", nullptr, SPD_MODE_THREADED, nullptr, 1, &error);
        const QString detail = QString::fromUtf8(error ? error : "");
        std::free(error);
        if (!m_connection) {
            fail(i18n("Could not connect to Speech Dispatcher: %1", detail));
            return false;
        }
        m_client = spd_get_client_id(m_connection);
        if (m_client < 0) {
            fail(i18n("Could not initialize the Speech Dispatcher connection."));
            return false;
        }
        {
            QMutexLocker lock(&callbackMutex);
            clients.insert(size_t(m_client), this);
        }
        m_connection->callback_begin = notification;
        m_connection->callback_end = notification;
        m_connection->callback_cancel = notification;
        m_connection->callback_pause = notification;
        m_connection->callback_resume = notification;
        m_connection->callback_im = indexNotification;
        if (spd_set_notification_on(m_connection, SPD_ALL) != 0 || spd_set_data_mode(m_connection, SPD_DATA_TEXT) != 0) {
            fail(i18n("Could not initialize the Speech Dispatcher connection."));
            return false;
        }
        char *language = spd_get_language(m_connection);
        m_defaultLanguage = QString::fromUtf8(language ? language : "");
        std::free(language);
        char *module = spd_get_output_module(m_connection);
        m_defaultModule = QString::fromUtf8(module ? module : "");
        std::free(module);
        return true;
    }

    void close()
    {
        {
            QMutexLocker lock(&callbackMutex);
            if (m_client >= 0) {
                clients.remove(size_t(m_client));
            }
        }
        m_client = -1;
        if (m_connection) {
            spd_close(m_connection);
            m_connection = nullptr;
        }
    }

    bool loadVoices()
    {
        m_voices.clear();
        char **modules = spd_list_modules(m_connection);
        char *original = spd_get_output_module(m_connection);
        if (modules) {
            for (auto module = modules; *module; ++module) {
                if (spd_set_output_module(m_connection, *module) != 0) {
                    continue;
                }
                SPDVoice **voices = spd_list_synthesis_voices(m_connection);
                if (!voices) {
                    continue;
                }
                for (auto voice = voices; *voice; ++voice) {
                    const QString moduleName = QString::fromUtf8(*module);
                    const QString name = QString::fromUtf8((*voice)->name);
                    const QString language = QString::fromUtf8((*voice)->language);
                    const QString id = QString::fromUtf8(QJsonDocument(QJsonArray{moduleName, name}).toJson(QJsonDocument::Compact));
                    m_voices.append(
                        QVariantMap{{QStringLiteral("id"), id},
                                    {QStringLiteral("name"), i18nc("speech voice, language, output module", "%1 (%2, %3)", name, language, moduleName)},
                                    {QStringLiteral("voice"), name},
                                    {QStringLiteral("language"), language},
                                    {QStringLiteral("module"), moduleName}});
                }
                free_spd_voices(voices);
            }
            free_spd_modules(modules);
        }
        if (original) {
            spd_set_output_module(m_connection, original);
            std::free(original);
        }
        Q_EMIT voicesLoaded(m_voices);
        if (m_voices.isEmpty()) {
            fail(i18n("Speech Dispatcher does not offer any voices. Check its output modules."));
            return false;
        }
        return true;
    }

    QVariantMap voiceFor(QString language) const
    {
        if (!m_preferredVoice.isEmpty()) {
            for (const auto &item : m_voices) {
                const auto voice = item.toMap();
                if (voice.value(QStringLiteral("id")).toString() == m_preferredVoice) {
                    return voice;
                }
            }
            return {};
        }
        if (language.isEmpty()) {
            language = QLocale::system().language() == QLocale::C ? m_defaultLanguage : QLocale::system().bcp47Name();
        }
        language = languageTag(language);
        const QString baseLanguage = language.section(QLatin1Char('-'), 0, 0);
        const QString preferredModule = (baseLanguage == QLatin1String("sa") || baseLanguage == QLatin1String("bn"))
            ? QStringLiteral("vits") : m_defaultModule;
        QVariantMap match;
        QVariantMap exactMatch;
        QVariantMap preferredMatch;
        for (const auto &item : m_voices) {
            const auto voice = item.toMap();
            const QString candidate = languageTag(voice.value(QStringLiteral("language")).toString());
            const bool preferred = voice.value(QStringLiteral("module")).toString() == preferredModule;
            if (candidate == language) {
                if (preferred) return voice;
                if (exactMatch.isEmpty()) exactMatch = voice;
            }
            if (candidate.section(QLatin1Char('-'), 0, 0) == language.section(QLatin1Char('-'), 0, 0)) {
                if (match.isEmpty()) match = voice;
                if (preferred && preferredMatch.isEmpty()) preferredMatch = voice;
            }
        }
        if (!preferredMatch.isEmpty()) return preferredMatch;
        return exactMatch.isEmpty() ? match : exactMatch;
    }

    static bool isInlineMath(const QVariantMap &segment)
    {
        return segment.value(QStringLiteral("type")).toString() == QLatin1String("math")
            && segment.value(QStringLiteral("inline")).toBool();
    }

    static bool isInlineSpeech(const QVariantMap &segment)
    {
        return isInlineMath(segment) || segment.value(QStringLiteral("type")).toString() == QLatin1String("reference")
            || segment.value(QStringLiteral("type")).toString() == QLatin1String("coordinate");
    }

    static bool canPrepare(const QVariantMap &segment)
    {
        return segment.value(QStringLiteral("type")).toString() == QLatin1String("math")
            ? isInlineMath(segment) : !segment.value(QStringLiteral("marks")).toList().isEmpty()
                || !segment.value(QStringLiteral("sentenceId")).toString().isEmpty();
    }

    QString pronouncedText(const QVariantMap &segment, const QString &voiceLanguage, qsizetype start = 0, qsizetype length = -1) const
    {
        const QString source = segment.value(QStringLiteral("text")).toString();
        const QString language = speechLanguage(segment);
        if (languageTag(language.isEmpty() ? voiceLanguage : language).section(QLatin1Char('-'), 0, 0) == QLatin1String("de")) {
            return pronounceSpeechText(expandGermanAbbreviations(source, start, length), voiceLanguage, segment);
        }
        const bool sanskrit = languageTag(voiceLanguage).section(QLatin1Char('-'), 0, 0) == QLatin1String("sa")
            && languageTag(speechLanguage(segment)).section(QLatin1Char('-'), 0, 0) == QLatin1String("sa");
        if (m_expandSanskritNumbers && sanskrit && segment.value(QStringLiteral("type")).toString() == QLatin1String("coordinate")
            && start == 0 && (length < 0 || length == source.size())) {
            const auto expanded = SanskritNumbers::coordinateForSpeech(segment.value(QStringLiteral("structureCoordinate")).toList());
            if (!expanded.isEmpty()) return expanded;
        }
        const QString text = m_expandSanskritNumbers && sanskrit
            ? SanskritNumbers::expandForSpeech(source, start, length) : source.mid(start, length);
        return pronounceSpeechText(text, voiceLanguage, segment);
    }

    QString markedText(const QString &voiceLanguage)
    {
        m_currentMarks.clear();
        QString ssml;
        QXmlStreamWriter writer(&ssml);
        writer.writeStartElement(QStringLiteral("speak"));
        // Match the native Piper module's speech-duration scaling. Keeping
        // the factor finite at -100 also bounds a 500 ms base pause to 1500 ms.
        const double durationFactor = m_rate < 0 ? 1.0 - m_rate / 50.0 : 1.0 - m_rate / 150.0;
        const int sentencePauseMs = qRound(m_sentencePauseMs * durationFactor);
        const int paragraphPauseMs = qRound(m_paragraphPauseMs * durationFactor);
        bool sentenceOpen = false;
        bool packageOpen = false;
        for (qsizetype index = 0; index < m_batch.size(); ++index) {
            const auto segment = m_batch.at(index).toMap();
            const QString text = segment.value(QStringLiteral("text")).toString();
            if (m_batch.size() > 1) {
                const auto previous = index ? m_batch.at(index - 1).toMap() : QVariantMap{};
                const QString sentenceId = segment.value(QStringLiteral("sentenceId")).toString();
                const bool newPackage = m_packageStarts.contains(index);
                // Splitting a long paragraph into packages is not a DOM
                // paragraph boundary: it keeps the ordinary sentence pause.
                const bool newParagraph = newPackage && index
                    && segment.value(QStringLiteral("blockId")) != previous.value(QStringLiteral("blockId"));
                const bool newSentence = !index || sentenceId.isEmpty()
                    || newPackage || sentenceId != previous.value(QStringLiteral("sentenceId")).toString();
                if (newSentence && sentenceOpen) {
                    writer.writeEndElement();
                    if (newPackage && packageOpen) {
                        writer.writeEndElement();
                        packageOpen = false;
                    }
                    // Keep the next segment's playback/word marks after its pause.
                    const int pauseMs = newParagraph ? paragraphPauseMs : sentencePauseMs;
                    if (pauseMs > 0) {
                        writer.writeEmptyElement(QStringLiteral("break"));
                        writer.writeAttribute(QStringLiteral("time"), QString::number(pauseMs) + QStringLiteral("ms"));
                    }
                }
                if (newPackage) {
                    writer.writeStartElement(QStringLiteral("p"));
                    packageOpen = true;
                }
                if (!newSentence && (isInlineSpeech(segment) || isInlineSpeech(previous))) {
                    const QString previousText = previous.value(QStringLiteral("text")).toString();
                    if (!previousText.isEmpty() && !text.isEmpty() && !previousText.back().isSpace()
                        && !text.front().isSpace() && !text.front().isPunct())
                        writer.writeCharacters(QStringLiteral(" "));
                }
                const QString name = QStringLiteral("arianna-segment-%1-%2").arg(m_session).arg(++m_markSerial);
                writer.writeEmptyElement(QStringLiteral("mark"));
                writer.writeAttribute(QStringLiteral("name"), name);
                m_currentMarks.insert(name, {index, {}});
                // Inline formulas and adjacent prose share one inference, while
                // retaining a segment mark for the whole-formula underline.
                if (newSentence) {
                    writer.writeStartElement(QStringLiteral("s"));
                    sentenceOpen = true;
                }
            }
            qsizetype position = 0;
            qsizetype previousEnd = 0;
            const auto boundary = [&text](qsizetype offset) {
                return offset <= 0 || offset >= text.size() || !text.at(offset).isLowSurrogate() || !text.at(offset - 1).isHighSurrogate();
            };
            for (const auto &value : segment.value(QStringLiteral("marks")).toList()) {
                const auto mark = value.toMap();
                bool validStart = false;
                bool validLength = false;
                const qlonglong start = mark.value(QStringLiteral("start")).toLongLong(&validStart);
                const qlonglong length = mark.value(QStringLiteral("length")).toLongLong(&validLength);
                if (!validStart || !validLength || start < previousEnd || start > text.size() || length <= 0
                    || length > text.size() - start || !boundary(start) || !boundary(start + length)) {
                    continue;
                }
                const QString name = QStringLiteral("arianna-%1-%2").arg(m_session).arg(++m_markSerial);
                writer.writeCharacters(pronouncedText(segment, voiceLanguage, position, start - position));
                writer.writeEmptyElement(QStringLiteral("mark"));
                writer.writeAttribute(QStringLiteral("name"), name);
                m_currentMarks.insert(name, {index, mark});
                position = start;
                previousEnd = start + length;
            }
            writer.writeCharacters(pronouncedText(segment, voiceLanguage, position));
        }
        if (sentenceOpen) writer.writeEndElement();
        if (packageOpen) writer.writeEndElement();
        writer.writeEndElement();
        if (writer.hasError()) {
            m_currentMarks.clear();
        }
        return m_currentMarks.isEmpty() ? pronouncedText(m_currentSegment, voiceLanguage) : ssml;
    }

    void next()
    {
        if (m_session != *m_generation) {
            return;
        }
        if (m_segments.isEmpty()) {
            if (m_hasMore) {
                if (!m_waiting) {
                    m_waiting = true;
                    Q_EMIT moreSegmentsRequested(m_session, false);
                }
                return;
            }
            if (m_repeatSelection && !m_selectionSegments.isEmpty()) {
                // An END received while paused must not restart the selection.
                if (m_paused) return;
                m_segments = m_selectionSegments;
                m_sentenceSegments.clear();
            } else {
                m_selectionSegments.clear();
                m_paused = false;
                Q_EMIT state(m_session, SpeechController::Idle);
                return;
            }
        }
        const auto segment = m_segments.takeFirst().toMap();
        m_currentSegment = segment;
        rememberSentence(segment);
        m_batch = {segment};
        m_packageStarts = {0};
        m_segmentIndex = 0;
        m_segmentStarted = false;
        const QString language = speechLanguage(segment);
        const auto voice = voiceFor(language);
        if (voice.isEmpty()) {
            fail(m_preferredVoice.isEmpty() ? i18n("No speech voice is available for the language '%1'. Choose a voice in Settings.",
                                                   language.isEmpty() ? QLocale::system().bcp47Name() : language)
                                            : i18n("The selected speech voice is no longer available. Choose a voice in Settings."));
            return;
        }
        const QString voiceLanguage = voice.value(QStringLiteral("language")).toString();
        // Native vits implements Stop; resume requeues the interrupted segment.
        m_restartOnPause = voice.value(QStringLiteral("module")).toString() == QLatin1String("vits");
        // Each logical package stays within one DOM block and 2048 pronounced
        // characters. Send a bounded window of paragraph packages together so
        // native Piper can prepare the following package during playback.
        // Headings, display math, language and chapter changes remain barriers.
        const QString blockId = segment.value(QStringLiteral("blockId")).toString();
        if (voice.value(QStringLiteral("module")).toString() == QLatin1String("piper")
            && !blockId.isEmpty()
            && canPrepare(segment)) {
            qsizetype characters = pronouncedText(segment, voiceLanguage).size();
            QString packageBlock = blockId;
            const bool preloadParagraphs = segment.value(QStringLiteral("blockTag")).toString() == QLatin1String("p");
            while (!m_segments.isEmpty()) {
                const auto candidate = m_segments.first().toMap();
                const qsizetype length = pronouncedText(candidate, voiceLanguage).size();
                const QString candidateBlock = candidate.value(QStringLiteral("blockId")).toString();
                if (!canPrepare(candidate) || candidateBlock.isEmpty()
                    || languageTag(speechLanguage(candidate)) != languageTag(language)
                    || candidate.value(QStringLiteral("href")) != segment.value(QStringLiteral("href"))
                    || voiceFor(speechLanguage(candidate)) != voice) break;
                if (characters + length > 2048 || candidateBlock != packageBlock) {
                    if (!preloadParagraphs || candidate.value(QStringLiteral("blockTag")).toString() != QLatin1String("p")
                        || m_packageStarts.size() >= 4 || characters > 2048 || length > 2048) break;
                    m_packageStarts.append(m_batch.size());
                    packageBlock = candidateBlock;
                    characters = 0;
                }
                m_batch.append(m_segments.takeFirst());
                characters += length;
            }
        }
        if (disconnected() || spd_set_output_module(m_connection, voice.value(QStringLiteral("module")).toString().toUtf8().constData()) != 0
            || spd_set_language(m_connection, voice.value(QStringLiteral("language")).toString().toUtf8().constData()) != 0
            || spd_set_synthesis_voice(m_connection, voice.value(QStringLiteral("voice")).toString().toUtf8().constData()) != 0
            || spd_set_voice_rate(m_connection, m_rate) != 0) {
            fail(i18n("Speech Dispatcher could not select the requested voice."));
            return;
        }
        if (m_session != *m_generation) {
            return;
        }
        QString text = markedText(voiceLanguage);
        if (m_currentMarks.isEmpty() && m_batch.size() > 1) {
            m_segments = m_batch.mid(1) + m_segments;
            m_batch = {segment};
        }
        const auto mode = m_currentMarks.isEmpty() ? SPD_DATA_TEXT : SPD_DATA_SSML;
        if (spd_set_data_mode(m_connection, mode) != 0) {
            // A server rejecting SSML can still speak the pronunciation aliases.
            m_currentMarks.clear();
            if (m_batch.size() > 1) {
                m_segments = m_batch.mid(1) + m_segments;
                m_batch = {segment};
            }
            text = pronouncedText(segment, voiceLanguage);
            if (spd_set_data_mode(m_connection, SPD_DATA_TEXT) != 0) {
                fail(i18n("Speech Dispatcher could not start the speech."));
                return;
            }
        }
        m_message = spd_say(m_connection, SPD_TEXT, text.toUtf8().constData());
        if (m_message < 0) {
            fail(i18n("Speech Dispatcher could not start the speech."));
            return;
        }
        Q_EMIT state(m_session, SpeechController::Speaking);
        // Extract the next DOM batch while this window is speaking. The reader
        // defers chapter navigation until a non-preload request, so preparing
        // text cannot turn the page or move the visible underline early.
        if (voice.value(QStringLiteral("module")).toString() == QLatin1String("piper")
            && m_segments.isEmpty() && m_hasMore && !m_waiting && !m_preloadEmpty) {
            m_waiting = true;
            Q_EMIT moreSegmentsRequested(m_session, true);
        }
    }

    void fail(const QString &message)
    {
        cancel();
        close();
        Q_EMIT failed(m_session, message);
    }

    std::shared_ptr<std::atomic<quint64>> m_generation;
    SPDConnection *m_connection = nullptr;
    int m_client = -1;
    int m_message = -1;
    quint64 m_session = 0;
    bool m_paused = false;
    bool m_restartOnPause = false;
    bool m_segmentStarted = false;
    bool m_hasMore = false;
    bool m_waiting = false;
    bool m_preloadEmpty = false;
    bool m_repeatSelection = false;
    qsizetype m_finishAfterSegment = -1;
    int m_rate = 0;
    int m_sentencePauseMs = 200;
    int m_paragraphPauseMs = 400;
    bool m_expandSanskritNumbers = false;
    QVariantMap m_currentSegment;
    struct SpeechMark {
        qsizetype segment;
        QVariantMap word;
    };
    QHash<QString, SpeechMark> m_currentMarks;
    QVariantList m_batch;
    QList<qsizetype> m_packageStarts;
    qsizetype m_segmentIndex = 0;
    quint64 m_markSerial = 0;
    QVariantList m_segments;
    QVariantList m_selectionSegments;
    QVariantList m_sentenceSegments;
    QVariantList m_voices;
    QString m_preferredVoice;
    QString m_defaultLanguage;
    QString m_defaultModule;
};

SpeechController::SpeechController(QObject *parent)
    : QObject(parent)
    , m_generation(std::make_shared<std::atomic<quint64>>(0))
{
    m_worker = new SpeechWorker(m_generation);
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &SpeechWorker::state, this, [this](quint64 generation, State state) {
        if (generation == *m_generation) {
            setState(state);
        }
    });
    connect(m_worker, &SpeechWorker::failed, this, [this](quint64 generation, const QString &message) {
        if (generation == *m_generation) {
            setState(Idle);
            setError(message);
            Q_EMIT errorOccurred(message);
        }
    });
    connect(m_worker, &SpeechWorker::voicesLoaded, this, [this](const QVariantList &voices) {
        m_voices = voices;
        Q_EMIT voicesChanged();
    });
    connect(m_worker, &SpeechWorker::segmentStarted, this, [this](quint64 generation, const QVariantMap &segment) {
        if (generation == *m_generation) {
            Q_EMIT segmentStarted(QString::number(generation), segment);
        }
    });
    connect(m_worker, &SpeechWorker::segmentFinished, this, [this](quint64 generation, const QString &segmentId) {
        if (generation == *m_generation) {
            Q_EMIT segmentFinished(QString::number(generation), segmentId);
        }
    });
    connect(m_worker, &SpeechWorker::indexMarkReached, this, [this](quint64 generation, const QString &segmentId, const QVariantMap &mark) {
        if (generation == *m_generation) {
            Q_EMIT indexMarkReached(QString::number(generation), segmentId, mark);
        }
    });
    connect(m_worker, &SpeechWorker::moreSegmentsRequested, this, [this](quint64 generation, bool preload) {
        if (generation == *m_generation) Q_EMIT moreSegmentsRequested(QString::number(generation), preload);
    });
    connect(m_worker, &SpeechWorker::readingExtended, this, [this](quint64 generation, const QString &requestId, const QVariantMap &lastSegment) {
        if (generation == *m_generation) Q_EMIT readingExtended(QString::number(generation), requestId, lastSegment);
    });
    connect(Config::self(), &Config::speechSentencePauseMsChanged, this, [this] {
        const int milliseconds = Config::speechSentencePauseMs();
        QMetaObject::invokeMethod(m_worker, [worker = m_worker, milliseconds] {
            worker->setSentencePauseMs(milliseconds);
        }, Qt::QueuedConnection);
    });
    connect(Config::self(), &Config::speechParagraphPauseMsChanged, this, [this] {
        const int milliseconds = Config::speechParagraphPauseMs();
        QMetaObject::invokeMethod(m_worker, [worker = m_worker, milliseconds] {
            worker->setParagraphPauseMs(milliseconds);
        }, Qt::QueuedConnection);
    });
    m_thread.start();
}

SpeechController::~SpeechController()
{
    ++*m_generation;
    QMetaObject::invokeMethod(m_worker, &SpeechWorker::shutdown, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

SpeechController::State SpeechController::state() const
{
    return m_state;
}
QVariantList SpeechController::voices() const
{
    return m_voices;
}
QString SpeechController::errorString() const
{
    return m_error;
}

int SpeechController::speechRate() const
{
    return m_speechRate;
}

bool SpeechController::repeatSelection() const
{
    return m_repeatSelection;
}

void SpeechController::setRepeatSelection(bool repeat)
{
    if (repeat == m_repeatSelection) return;
    m_repeatSelection = repeat;
    Q_EMIT repeatSelectionChanged();
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, repeat] {
        worker->setRepeatSelection(repeat);
    }, Qt::QueuedConnection);
}

QString SpeechController::sessionId() const
{
    return QString::number(*m_generation);
}

void SpeechController::setState(State state)
{
    if (m_state != state) {
        m_state = state;
        Q_EMIT stateChanged();
    }
}

void SpeechController::setError(const QString &message)
{
    if (m_error != message) {
        m_error = message;
        Q_EMIT errorStringChanged();
    }
}

void SpeechController::speak(const QString &text, const QString &language)
{
    speakSegments({QVariantMap{{QStringLiteral("text"), text}, {QStringLiteral("lang"), language}}});
}

void SpeechController::speakSegments(const QVariantList &segments)
{
    startSegments(segments, false, true);
}

void SpeechController::startReading(const QVariantList &segments, bool hasMore)
{
    startSegments(segments, hasMore, false);
}

void SpeechController::continueReading(const QString &sessionId, const QVariantList &segments, bool hasMore)
{
    if (sessionId != this->sessionId() || m_state == Idle) return;
    QVariantList spoken;
    for (const auto &segment : segments) {
        if (!segment.toMap().value(QStringLiteral("text")).toString().trimmed().isEmpty()) spoken.append(segment);
    }
    const quint64 generation = *m_generation;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, generation, spoken, hasMore] {
        worker->append(generation, spoken, hasMore);
    }, Qt::QueuedConnection);
}

void SpeechController::extendReading(const QString &requestId)
{
    if (m_state == Idle) return;
    const quint64 generation = *m_generation;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, generation, requestId] {
        worker->extendReading(generation, requestId);
    }, Qt::QueuedConnection);
}

void SpeechController::finishSentence(bool repeat)
{
    setRepeatSelection(repeat);
    // The sentence may have ended between the Once and Repeat clicks. Its
    // original pieces remain available until Stop or a new session invalidates them.
    const quint64 generation = *m_generation;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, generation, repeat] {
        worker->finishSentence(generation, repeat);
    }, Qt::QueuedConnection);
}

void SpeechController::startSegments(const QVariantList &segments, bool hasMore, bool selection)
{
    QVariantList spoken;
    for (const auto &segment : segments) {
        auto item = segment.toMap();
        if (!item.value(QStringLiteral("text")).toString().trimmed().isEmpty()) {
            if (item.value(QStringLiteral("id")).toString().isEmpty()) {
                item.insert(QStringLiteral("id"), QString::number(spoken.size()));
            }
            spoken.append(item);
        }
    }
    if (spoken.isEmpty() && !hasMore) {
        return;
    }
    const quint64 generation = ++*m_generation;
    Q_EMIT sessionChanged();
    const QString voice = Config::speechVoice();
    const int rate = m_speechRate;
    const int sentencePauseMs = Config::speechSentencePauseMs();
    const int paragraphPauseMs = Config::speechParagraphPauseMs();
    const bool expandSanskritNumbers = Config::speechExpandSanskritNumbers();
    setError({});
    setState(Speaking);
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, generation, spoken, voice, rate, sentencePauseMs, paragraphPauseMs, expandSanskritNumbers, hasMore, selection] {
            worker->play(generation, spoken, voice, rate, sentencePauseMs, paragraphPauseMs, expandSanskritNumbers, hasMore, selection);
        },
        Qt::QueuedConnection);
}

void SpeechController::stop()
{
    ++*m_generation;
    Q_EMIT sessionChanged();
    setState(Idle);
    QMetaObject::invokeMethod(m_worker, &SpeechWorker::cancel, Qt::QueuedConnection);
}

void SpeechController::pause()
{
    const quint64 generation = *m_generation;
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, generation] {
            worker->pause(generation);
        },
        Qt::QueuedConnection);
}

void SpeechController::resume()
{
    const quint64 generation = *m_generation;
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, generation] {
            worker->resume(generation);
        },
        Qt::QueuedConnection);
}

void SpeechController::adjustSpeechRate(int delta)
{
    const int rate = qBound(-100, m_speechRate + delta, 100);
    if (rate == m_speechRate) {
        return;
    }
    m_speechRate = rate;
    Q_EMIT speechRateChanged();
    const quint64 generation = *m_generation;
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, generation, rate] {
            worker->setRate(generation, rate);
        },
        Qt::QueuedConnection);
}

void SpeechController::refreshVoices()
{
    if (m_state != Idle) {
        return;
    }
    setError({});
    const quint64 generation = *m_generation;
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, generation] {
            worker->refresh(generation);
        },
        Qt::QueuedConnection);
}

#include "speechcontroller.moc"
