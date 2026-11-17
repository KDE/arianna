#ifndef TRANSLATOR_H
#define TRANSLATOR_H

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <qqmlintegration.h>

class Translator : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit Translator(QObject *parent = nullptr);
    // Takes ownership of the network manager.
    Translator(QNetworkAccessManager *networkManager, QObject *parent);
    virtual ~Translator() = default;

    Q_INVOKABLE int translate(const QString &sourceText);
    Q_INVOKABLE int translateSegments(const QVariantList &segments);

    enum TranslatorEngine {
        Google = 0,
        DeepL = 1
    };
    Q_ENUM(TranslatorEngine)

Q_SIGNALS:
    // Escaped rich text; an optional transcription is italic and bracketed.
    void translationReady(int requestId, const QString &translatedText, bool hasTranscription);

private:
    struct TranslationRequest {
        int id;
        TranslatorEngine engine;
        QString apiKey;
        QString targetLanguage;
        QString transcription;
        QString transcriptionError;
    };

    QNetworkAccessManager *networkManager;
    int nextRequestId = 0;
    TranslationRequest makeRequest(const QString &sourceText);
    QString translationFromReply(QNetworkReply *reply, const TranslationRequest &request) const;
    QNetworkReply *translateByDeepL(const QString &sourceText, const TranslationRequest &request);
    QNetworkReply *translateByGoogle(const QString &sourceText, const TranslationRequest &request);
    void handleNetworkReply(QNetworkReply *reply, const TranslationRequest &request);
    void finishTranslation(const TranslationRequest &request, const QString &translatedText);
};

#endif // TRANSLATOR_H
