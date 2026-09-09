// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QObject>
#include <QSet>
#include <QSizeF>
#include <QUrl>
#include <qqmlintegration.h>

#include <memory>

#include <core/document.h>
#include <core/observer.h>

namespace Okular
{
class Action;
class RegularAreaRect;
}

class OkularPdfDocumentObserver : public QObject, public Okular::DocumentObserver
{
    Q_OBJECT

public:
    explicit OkularPdfDocumentObserver(Okular::Document *document);
    ~OkularPdfDocumentObserver() override;

    void notifyPageChanged(int page, int flags) override;

Q_SIGNALS:
    void pageChanged(int page, int flags);

private:
    Okular::Document *m_document = nullptr;
};

class OkularPdfDocument : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool opened READ opened NOTIFY openedChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool needsPassword READ needsPassword NOTIFY needsPasswordChanged)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY pageCountChanged)
    Q_PROPERTY(int currentPage READ currentPage WRITE setCurrentPage NOTIFY currentPageChanged)
    Q_PROPERTY(QString password READ password WRITE setPassword NOTIFY passwordChanged)
    Q_PROPERTY(QString error READ errorString NOTIFY errorChanged)
    Q_PROPERTY(QString title READ title NOTIFY metadataChanged)
    Q_PROPERTY(QString author READ author NOTIFY metadataChanged)
    Q_PROPERTY(QString subject READ subject NOTIFY metadataChanged)
    Q_PROPERTY(QString keywords READ keywords NOTIFY metadataChanged)
    Q_PROPERTY(QString creator READ creator NOTIFY metadataChanged)
    Q_PROPERTY(QString producer READ producer NOTIFY metadataChanged)
    Q_PROPERTY(QString creationDate READ creationDate NOTIFY metadataChanged)
    Q_PROPERTY(QString selectedText READ selectedText NOTIFY selectedTextChanged)
    Q_PROPERTY(bool hasTextSelection READ hasTextSelection NOTIFY selectedTextChanged)
    Q_PROPERTY(QString tableOfContentsJson READ tableOfContentsJson NOTIFY tableOfContentsChanged)

public:
    enum Status {
        Null,
        Loading,
        Ready,
        Error,
        NeedsPassword,
    };
    Q_ENUM(Status)

    explicit OkularPdfDocument(QObject *parent = nullptr);
    ~OkularPdfDocument() override;

    QUrl source() const;
    void setSource(const QUrl &source);

    Status status() const;
    bool opened() const;
    bool loading() const;
    bool needsPassword() const;
    int pageCount() const;

    int currentPage() const;
    void setCurrentPage(int page);

    QString password() const;
    void setPassword(const QString &password);

    QString errorString() const;
    QString title() const;
    QString author() const;
    QString subject() const;
    QString keywords() const;
    QString creator() const;
    QString producer() const;
    QString creationDate() const;
    QString selectedText() const;
    bool hasTextSelection() const;
    QString tableOfContentsJson() const;

    Q_INVOKABLE QSizeF pagePointSize(int page) const;
    Q_INVOKABLE void clearTextSelection();
    Q_INVOKABLE void copySelectedText(bool removeLineBreaks) const;
    Q_INVOKABLE QString selectedTextForTranslation() const;
    Q_INVOKABLE QVariantMap writeTableOfContentsToPdf(const QString &tableOfContentsJson, const QString &targetFileName = QString());
    Q_INVOKABLE void reload();

    Okular::Document *document() const;
    OkularPdfDocumentObserver *pageviewObserver() const;
    QString internalLocationForAction(const Okular::Action *action) const;
    QString locationForPagePosition(int page, double normalizedX, double normalizedY, Okular::DocumentViewport::Position position) const;
    void setTextSelection(int page, std::unique_ptr<Okular::RegularAreaRect> &&rect);

Q_SIGNALS:
    void sourceChanged();
    void statusChanged();
    void openedChanged();
    void loadingChanged();
    void needsPasswordChanged();
    void pageCountChanged();
    void currentPageChanged();
    void passwordChanged();
    void errorChanged();
    void metadataChanged();
    void selectedTextChanged();
    void tableOfContentsChanged();
    void passwordRequired();
    void pageChanged(int page, int flags);

private:
    void openSource(const QString &password = QString());
    void clearDocumentState();
    void updateMetadata();
    void updateTableOfContents();
    void setStatus(Status status);
    void setPageCount(int pageCount);
    void setNeedsPassword(bool needsPassword);
    void setErrorString(const QString &errorString);
    void setCurrentPageValue(int page);

    QUrl m_source;
    std::unique_ptr<Okular::Document> m_document;
    std::unique_ptr<OkularPdfDocumentObserver> m_pageviewObserver;
    Status m_status = Null;
    bool m_needsPassword = false;
    int m_pageCount = 0;
    int m_currentPage = 0;
    QString m_password;
    QString m_errorString;
    QString m_title;
    QString m_author;
    QString m_subject;
    QString m_keywords;
    QString m_creator;
    QString m_producer;
    QString m_creationDate;
    QSet<int> m_pagesWithTextSelection;
    QString m_tableOfContentsJson = QStringLiteral("[]");
};
