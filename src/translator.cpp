#include "translator.h"
#include "config.h"
#include "transliterator.h"

#include <KLocalizedString>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>
#include <QUrlQuery>

#include <limits>
#include <memory>

Translator::Translator(QObject *parent)
    : Translator(new QNetworkAccessManager, parent)
{
}

Translator::Translator(QNetworkAccessManager *manager, QObject *parent)
    : QObject(parent)
    , networkManager(manager)
{
    networkManager->setParent(this);
    networkManager->setTransferTimeout(30000);
}

QNetworkReply *Translator::translateByGoogle(const QString &sourceText, const TranslationRequest &translation)
{
    if (!translation.apiKey.isEmpty()) {
        QUrl url(QStringLiteral("https://translation.googleapis.com/language/translate/v2"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("key"), translation.apiKey);
        query.addQueryItem(QStringLiteral("q"), sourceText);
        query.addQueryItem(QStringLiteral("target"), translation.targetLanguage);
        query.addQueryItem(QStringLiteral("format"), QStringLiteral("text"));
        url.setQuery(query);
        return networkManager->get(QNetworkRequest(url));
    }

    QUrl url(QStringLiteral("https://translate.googleapis.com/translate_a/single"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client"), QStringLiteral("gtx"));
    query.addQueryItem(QStringLiteral("ie"), QStringLiteral("UTF-8"));
    query.addQueryItem(QStringLiteral("sl"), QStringLiteral("auto"));
    query.addQueryItem(QStringLiteral("tl"), translation.targetLanguage);
    query.addQueryItem(QStringLiteral("dt"), QStringLiteral("t"));
    query.addQueryItem(QStringLiteral("q"), sourceText);
    url.setQuery(query);
    return networkManager->get(QNetworkRequest(url));
}

QNetworkReply *Translator::translateByDeepL(const QString &sourceText, const TranslationRequest &translation)
{
    const QString authKey = translation.apiKey.trimmed();
    const bool isFreeApiKey = authKey.endsWith(QStringLiteral(":fx"));
    const QUrl url(isFreeApiKey ? QStringLiteral("https://api-free.deepl.com/v2/translate") : QStringLiteral("https://api.deepl.com/v2/translate"));

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("text"), sourceText);
    query.addQueryItem(QStringLiteral("target_lang"), translation.targetLanguage);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader("Authorization", "DeepL-Auth-Key " + authKey.toUtf8());
    return networkManager->post(request, query.query(QUrl::FullyEncoded).toUtf8());
}

Translator::TranslationRequest Translator::makeRequest(const QString &sourceText)
{
    nextRequestId = nextRequestId == std::numeric_limits<int>::max() ? 1 : nextRequestId + 1;
    TranslationRequest request;
    request.id = nextRequestId;
    request.engine = static_cast<TranslatorEngine>(Config::translatorEngine());
    request.apiKey = request.engine == DeepL ? Config::deeplApiKey() : Config::googleApiKey();
    request.targetLanguage = Config::targetLanguage().isEmpty() ? QStringLiteral("DE") : Config::targetLanguage();

    if (Config::transcribeDevanagari() && Transliterator::containsDevanagari(sourceText)) {
        const auto transcription = Transliterator::devanagariToIast(sourceText);
        if (transcription) {
            request.transcription = *transcription;
        } else {
            request.transcriptionError = i18n("Devanagari transcription failed.");
        }
    }
    return request;
}

int Translator::translate(const QString &sourceText)
{
    const TranslationRequest request = makeRequest(sourceText);
    if (sourceText.trimmed().isEmpty()) {
        finishTranslation(request, QString());
        return request.id;
    }
    if (request.engine == DeepL && request.apiKey.trimmed().isEmpty()) {
        finishTranslation(request, i18n("Error: DeepL API key is missing"));
        return request.id;
    }

    QNetworkReply *reply = request.engine == DeepL ? translateByDeepL(sourceText, request) : translateByGoogle(sourceText, request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, request]() {
        handleNetworkReply(reply, request);
    });
    return request.id;
}

int Translator::translateSegments(const QVariantList &segments)
{
    QString sourceText;
    for (const QVariant &segment : segments) {
        sourceText += segment.toMap().value(QStringLiteral("text")).toString();
    }
    const TranslationRequest request = makeRequest(sourceText);
    if (sourceText.trimmed().isEmpty()) {
        finishTranslation(request, QString());
        return request.id;
    }
    if (request.engine == DeepL && request.apiKey.trimmed().isEmpty()) {
        finishTranslation(request, i18n("Error: DeepL API key is missing"));
        return request.id;
    }

    // Translate each language run separately. The providers detect the source
    // language for each request; the original spacing joins the results again.
    struct Batch {
        QStringList parts;
        int remaining = 0;
    };
    const auto batch = std::make_shared<Batch>();
    batch->parts.resize(segments.size());
    for (qsizetype index = 0; index < segments.size(); ++index) {
        const QString text = segments.at(index).toMap().value(QStringLiteral("text")).toString();
        qsizetype first = 0;
        while (first < text.size() && text.at(first).isSpace()) {
            ++first;
        }
        if (first == text.size()) {
            batch->parts[index] = text;
            continue;
        }
        qsizetype last = text.size();
        while (last > first && text.at(last - 1).isSpace()) {
            --last;
        }
        const QString prefix = text.left(first);
        const QString suffix = text.mid(last);
        const QString core = text.mid(first, last - first);
        ++batch->remaining;
        QNetworkReply *reply = request.engine == DeepL ? translateByDeepL(core, request) : translateByGoogle(core, request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, request, batch, index, prefix, suffix]() {
            batch->parts[index] = prefix + translationFromReply(reply, request) + suffix;
            reply->deleteLater();
            if (--batch->remaining == 0) {
                finishTranslation(request, batch->parts.join(QString()));
            }
        });
    }
    return request.id;
}

QString Translator::translationFromReply(QNetworkReply *reply, const TranslationRequest &request) const
{
    QString translatedText;
    if (reply->error() == QNetworkReply::NoError) {
        const QJsonDocument jsonDoc = QJsonDocument::fromJson(reply->readAll());
        if (request.engine == DeepL) {
            const QJsonArray translations = jsonDoc.object().value(QStringLiteral("translations")).toArray();
            if (!translations.isEmpty()) {
                translatedText = translations.first().toObject().value(QStringLiteral("text")).toString();
            }
        } else if (!request.apiKey.isEmpty()) {
            const QJsonArray translations = jsonDoc.object().value(QStringLiteral("data")).toObject().value(QStringLiteral("translations")).toArray();
            for (const QJsonValue &translation : translations) {
                translatedText += translation.toObject().value(QStringLiteral("translatedText")).toString();
            }
        } else {
            const QJsonArray response = jsonDoc.array();
            const QJsonArray translations = response.isEmpty() ? QJsonArray() : response.first().toArray();
            for (const QJsonValue &translation : translations) {
                const QJsonArray segment = translation.toArray();
                if (!segment.isEmpty()) {
                    translatedText += segment.first().toString();
                }
            }
        }
        if (translatedText.isEmpty()) {
            translatedText = i18n("No translation was returned.");
        }
    } else {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString response = QString::fromUtf8(reply->readAll()).trimmed();
        translatedText = QStringLiteral("Error");
        if (statusCode > 0) {
            translatedText += QStringLiteral(" %1").arg(statusCode);
        }
        translatedText += QStringLiteral(": ") + reply->errorString();
        if (!response.isEmpty()) {
            translatedText += QStringLiteral(" - ") + response;
        }
    }
    return translatedText;
}

void Translator::handleNetworkReply(QNetworkReply *reply, const TranslationRequest &request)
{
    finishTranslation(request, translationFromReply(reply, request));
    reply->deleteLater();
}

void Translator::finishTranslation(const TranslationRequest &request, const QString &translatedText)
{
    const auto escapedText = [](const QString &text) {
        return text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    };
    QString result = escapedText(translatedText);
    if (!request.transcription.isEmpty()) {
        result = QStringLiteral("<i>[") + escapedText(request.transcription) + QStringLiteral("]</i><br><br>") + result;
    } else if (!request.transcriptionError.isEmpty()) {
        result = escapedText(request.transcriptionError) + QStringLiteral("<br><br>") + result;
    }
    if (!result.isEmpty()) {
        result = QStringLiteral("<span style=\"white-space: pre-wrap;\">") + result + QStringLiteral("</span>");
    }
    // Always deliver asynchronously, including local errors, so QML has stored
    // the returned request ID before receiving its result.
    QTimer::singleShot(0, this, [this, id = request.id, result, hasTranscription = !request.transcription.isEmpty()]() {
        Q_EMIT translationReady(id, result, hasTranscription);
    });
}

#include "moc_translator.cpp"
