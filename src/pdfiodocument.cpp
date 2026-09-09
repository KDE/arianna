// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "pdfiodocument.h"

#include "pdfwatermarkfilter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <KLocalizedString>

#include <QDebug>

PdfIoDocument::PdfIoDocument(QObject *parent)
    : QObject(parent)
    , m_networkAccessManager(this)
{
}

PdfIoDocument::~PdfIoDocument()
{
    abortNetworkLoad();
}

QUrl PdfIoDocument::source() const
{
    return m_source;
}

void PdfIoDocument::setSource(const QUrl &source)
{
    if (m_source == source) {
        return;
    }

    m_source = source;
    Q_EMIT sourceChanged();
    loadSource();
}

QObject *PdfIoDocument::target() const
{
    return m_target;
}

void PdfIoDocument::setTarget(QObject *target)
{
    if (m_target == target) {
        return;
    }

    m_target = target;
    Q_EMIT targetChanged();
    if (!m_source.isEmpty()) {
        loadSource();
    }
}

bool PdfIoDocument::loading() const
{
    return m_loading;
}

QString PdfIoDocument::errorString() const
{
    return m_errorString;
}

bool PdfIoDocument::watermarkFilterEnabled() const
{
    return m_watermarkFilterEnabled;
}

void PdfIoDocument::setWatermarkFilterEnabled(bool enabled)
{
    if (m_watermarkFilterEnabled == enabled) {
        return;
    }

    m_watermarkFilterEnabled = enabled;
    Q_EMIT watermarkFilterEnabledChanged();
    if (!m_source.isEmpty() && m_target) {
        loadSource();
    }
}

QString PdfIoDocument::watermarkFilterPattern() const
{
    return m_watermarkFilterPattern;
}

void PdfIoDocument::setWatermarkFilterPattern(const QString &pattern)
{
    if (m_watermarkFilterPattern == pattern) {
        return;
    }

    m_watermarkFilterPattern = pattern;
    Q_EMIT watermarkFilterPatternChanged();
    if (m_watermarkFilterEnabled && !m_source.isEmpty() && m_target) {
        loadSource();
    }
}

void PdfIoDocument::reload()
{
    loadSource();
}

void PdfIoDocument::loadSource()
{
    abortNetworkLoad();
    m_errorString.clear();
    Q_EMIT errorChanged();

    if (m_source.isEmpty() || !m_target) {
        setTargetSource(QUrl());
        clearTemporaryFile();
        setLoading(false);
        return;
    }

    if (m_source.scheme() == QLatin1String("http") || m_source.scheme() == QLatin1String("https")) {
        loadNetworkFile(m_source);
        return;
    }

    const QString fileName = m_source.isLocalFile() ? m_source.toLocalFile() : m_source.toString(QUrl::PreferLocalFile);
    loadLocalFile(fileName);
}

void PdfIoDocument::loadLocalFile(const QString &fileName)
{
    if (!QFileInfo::exists(fileName)) {
        setTargetSource(QUrl());
        clearTemporaryFile();
        setNetworkError(i18n("The input PDF does not exist."));
        return;
    }

    setLoading(m_watermarkFilterEnabled);
    setTargetSource(filteredSourceForFile(fileName));
    setLoading(false);
}

void PdfIoDocument::loadNetworkFile(const QUrl &url)
{
    if (!m_target) {
        setNetworkError(i18n("The PDF document object is not available"));
        return;
    }

    setTargetSource(QUrl());
    clearTemporaryFile();
    setLoading(true);

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    auto *reply = m_networkAccessManager.get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        finishNetworkLoad(reply);
    });
}

void PdfIoDocument::finishNetworkLoad(QNetworkReply *reply)
{
    if (reply != m_reply) {
        reply->deleteLater();
        return;
    }

    m_reply = nullptr;
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || httpStatus >= 400) {
        const QString message = reply->error() != QNetworkReply::NoError ? reply->errorString() : i18n("HTTP error %1 while loading PDF", httpStatus);
        reply->deleteLater();
        setNetworkError(message);
        return;
    }

    const QByteArray pdfData = reply->readAll();
    reply->deleteLater();
    if (pdfData.isEmpty()) {
        setNetworkError(i18n("The PDF response was empty"));
        return;
    }

    QString temporaryFileError;
    auto temporaryFile = createTemporaryPdfFile(QStringLiteral("arianna-pdf"), &temporaryFileError);
    if (!temporaryFile) {
        setNetworkError(temporaryFileError);
        return;
    }
    if (temporaryFile->write(pdfData) != pdfData.size() || !temporaryFile->flush()) {
        setNetworkError(i18n("Unable to write the temporary PDF file"));
        return;
    }
    const QString temporaryFileName = temporaryFile->fileName();
    temporaryFile->close();

    m_errorString.clear();
    Q_EMIT errorChanged();

    m_temporaryFile = std::move(temporaryFile);
    setTargetSource(filteredSourceForFile(temporaryFileName, &m_temporaryFile));
    setLoading(false);
}

QUrl PdfIoDocument::filteredSourceForFile(const QString &fileName, std::unique_ptr<QTemporaryFile> *sourceTemporaryFile)
{
    if (!m_watermarkFilterEnabled) {
        if (!sourceTemporaryFile) {
            clearTemporaryFile();
        }
        return QUrl::fromLocalFile(fileName);
    }

    QString temporaryFileError;
    auto filteredFile = createTemporaryPdfFile(QStringLiteral("arianna-filtered-pdf"), &temporaryFileError);
    if (!filteredFile) {
        qWarning() << "Unable to create filtered PDF file:" << temporaryFileError;
        if (!sourceTemporaryFile) {
            clearTemporaryFile();
        }
        return QUrl::fromLocalFile(fileName);
    }

    const QString filteredFileName = filteredFile->fileName();
    filteredFile->close();
    QFile::remove(filteredFileName);

    const PdfWatermarkFilterResult filterResult = PdfWatermarkFilter::filterFile(fileName, filteredFileName, m_watermarkFilterPattern.trimmed());
    if (!filterResult.filtered) {
        if (!filterResult.errorString.isEmpty()) {
            qWarning() << "PDF watermark filter not applied:" << filterResult.errorString;
        } else {
            qDebug() << "PDF watermark marker not found:" << fileName;
        }
        if (!sourceTemporaryFile) {
            clearTemporaryFile();
        }
        return QUrl::fromLocalFile(fileName);
    }

    if (sourceTemporaryFile) {
        *sourceTemporaryFile = std::move(filteredFile);
    } else {
        m_temporaryFile = std::move(filteredFile);
    }
    qDebug() << "PDF watermark filter removed" << filterResult.occurrences << "object(s):" << fileName;
    return QUrl::fromLocalFile(filteredFileName);
}

std::unique_ptr<QTemporaryFile> PdfIoDocument::createTemporaryPdfFile(const QString &prefix, QString *errorString) const
{
    auto temporaryFile = std::make_unique<QTemporaryFile>(QDir::temp().filePath(prefix + QStringLiteral("-XXXXXX.pdf")));
    temporaryFile->setAutoRemove(true);
    if (!temporaryFile->open()) {
        if (errorString) {
            *errorString = i18n("Unable to create a temporary PDF file");
        }
        return {};
    }
    return temporaryFile;
}

void PdfIoDocument::abortNetworkLoad()
{
    if (!m_reply) {
        return;
    }

    auto *reply = m_reply.data();
    m_reply = nullptr;
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
}

void PdfIoDocument::clearTemporaryFile()
{
    m_temporaryFile.reset();
}

void PdfIoDocument::setNetworkError(const QString &message)
{
    setTargetSource(QUrl());
    clearTemporaryFile();
    m_errorString = message;
    setLoading(false);
    Q_EMIT errorChanged();
}

void PdfIoDocument::setLoading(bool loading)
{
    if (m_loading == loading) {
        return;
    }

    m_loading = loading;
    Q_EMIT loadingChanged();
}

void PdfIoDocument::setTargetSource(const QUrl &source)
{
    if (m_target) {
        m_target->setProperty("source", source);
    }
}
