// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QObject>
#include <QThread>
#include <QVariantList>
#include <qqmlintegration.h>

#include <atomic>
#include <memory>

class SpeechWorker;

// Each reader owns its session. Speech Dispatcher commands only affect this connection.
class SpeechController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QVariantList voices READ voices NOTIFY voicesChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(int speechRate READ speechRate NOTIFY speechRateChanged)
    Q_PROPERTY(bool repeatSelection READ repeatSelection WRITE setRepeatSelection NOTIFY repeatSelectionChanged)
    Q_PROPERTY(QString sessionId READ sessionId NOTIFY sessionChanged)

public:
    enum State {
        Idle,
        Speaking,
        Paused
    };
    Q_ENUM(State)

    explicit SpeechController(QObject *parent = nullptr);
    ~SpeechController() override;

    State state() const;
    QVariantList voices() const;
    QString errorString() const;
    int speechRate() const;
    bool repeatSelection() const;
    void setRepeatSelection(bool repeat);
    QString sessionId() const;

    Q_INVOKABLE void speak(const QString &text, const QString &language = QString());
    Q_INVOKABLE void speakSegments(const QVariantList &segments);
    Q_INVOKABLE void startReading(const QVariantList &segments, bool hasMore);
    Q_INVOKABLE void continueReading(const QString &sessionId, const QVariantList &segments, bool hasMore);
    Q_INVOKABLE void extendReading(const QString &requestId);
    Q_INVOKABLE void finishSentence(bool repeat);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void adjustSpeechRate(int delta);
    Q_INVOKABLE void refreshVoices();

Q_SIGNALS:
    void stateChanged();
    void voicesChanged();
    void errorStringChanged();
    void speechRateChanged();
    void repeatSelectionChanged();
    void sessionChanged();
    void segmentStarted(const QString &sessionId, const QVariantMap &segment);
    void segmentFinished(const QString &sessionId, const QString &segmentId);
    void indexMarkReached(const QString &sessionId, const QString &segmentId, const QVariantMap &mark);
    void moreSegmentsRequested(const QString &sessionId, bool preload);
    void readingExtended(const QString &sessionId, const QString &requestId, const QVariantMap &lastSegment);
    void errorOccurred(const QString &message);

private:
    void setState(State state);
    void setError(const QString &message);
    void startSegments(const QVariantList &segments, bool hasMore, bool selection);

    QThread m_thread;
    SpeechWorker *m_worker;
    std::shared_ptr<std::atomic<quint64>> m_generation;
    State m_state = Idle;
    QVariantList m_voices;
    QString m_error;
    int m_speechRate = 0;
    bool m_repeatSelection = false;
};
