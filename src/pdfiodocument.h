// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "config.h"

#include <QNetworkAccessManager>
#include <QPointer>
#include <QTemporaryFile>
#include <QUrl>
#include <qqmlintegration.h>

#include <memory>

class QNetworkReply;

class PdfIoDocument : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QObject *target READ target WRITE setTarget NOTIFY targetChanged)
    Q_PROPERTY(bool watermarkFilterEnabled READ watermarkFilterEnabled WRITE setWatermarkFilterEnabled NOTIFY watermarkFilterEnabledChanged)
    Q_PROPERTY(QString watermarkFilterPattern READ watermarkFilterPattern WRITE setWatermarkFilterPattern NOTIFY watermarkFilterPatternChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString error READ errorString NOTIFY errorChanged)

public:
    explicit PdfIoDocument(QObject *parent = nullptr);
    ~PdfIoDocument() override;

    QUrl source() const;
    void setSource(const QUrl &source);

    QObject *target() const;
    void setTarget(QObject *target);

    bool watermarkFilterEnabled() const;
    void setWatermarkFilterEnabled(bool enabled);

    QString watermarkFilterPattern() const;
    void setWatermarkFilterPattern(const QString &pattern);

    bool loading() const;
    QString errorString() const;

    Q_INVOKABLE void reload();

Q_SIGNALS:
    void sourceChanged();
    void targetChanged();
    void watermarkFilterEnabledChanged();
    void watermarkFilterPatternChanged();
    void loadingChanged();
    void errorChanged();

private:
    void loadSource();
    void loadLocalFile(const QString &fileName);
    void loadNetworkFile(const QUrl &url);
    void finishNetworkLoad(QNetworkReply *reply);
    QUrl filteredSourceForFile(const QString &fileName, std::unique_ptr<QTemporaryFile> *sourceTemporaryFile = nullptr);
    std::unique_ptr<QTemporaryFile> createTemporaryPdfFile(const QString &prefix, QString *errorString) const;
    void abortNetworkLoad();
    void clearTemporaryFile();
    void setNetworkError(const QString &message);
    void setLoading(bool loading);
    void setTargetSource(const QUrl &source);

    QUrl m_source;
    QPointer<QObject> m_target;
    QNetworkAccessManager m_networkAccessManager;
    QPointer<QNetworkReply> m_reply;
    std::unique_ptr<QTemporaryFile> m_temporaryFile;
    bool m_watermarkFilterEnabled = false;
    QString m_watermarkFilterPattern = Config::defaultPdfWatermarkFilterPatternValue();
    bool m_loading = false;
    QString m_errorString;
};
