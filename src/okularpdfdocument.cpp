// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

#include "okularpdfdocument.h"

#include "okularpdfsupport.h"

#include <QClipboard>
#include <QDebug>
#include <QDir>
#include <QDomElement>
#include <QDomNode>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QStringList>
#include <QTemporaryFile>
#include <QVector>

#include <KLocalizedString>

#include <algorithm>
#include <cmath>

#include <core/action.h>
#include <core/misc.h>
#include <core/page.h>
#include <core/textpage.h>

#include <podofo/auxiliary/StreamDevice.h>
#include <podofo/main/PdfDestination.h>
#include <podofo/main/PdfMemDocument.h>
#include <podofo/main/PdfOutlines.h>

namespace
{
PoDoFo::PdfString pdfStringFromQString(const QString &value)
{
    return PoDoFo::PdfString(value.toStdString());
}

int pageFromPdfLocation(const QJsonValue &value)
{
    if (!value.isString()) {
        return -1;
    }

    const QJsonDocument locationDocument = QJsonDocument::fromJson(value.toString().toUtf8());
    if (!locationDocument.isObject()) {
        return -1;
    }

    const QJsonObject location = locationDocument.object();
    if (location.value(QStringLiteral("page")).isDouble()) {
        return location.value(QStringLiteral("page")).toInt(-1);
    }
    if (location.value(QStringLiteral("pageNumber")).isDouble()) {
        return location.value(QStringLiteral("pageNumber")).toInt(0) - 1;
    }
    return -1;
}

PoDoFo::PdfOutlineItem *lastPdfOutlineSibling(PoDoFo::PdfOutlineItem *item)
{
    while (item && item->Next()) {
        item = item->Next();
    }
    return item;
}

void appendPdfOutlineItems(PoDoFo::PdfOutlineItem *parent, PoDoFo::PdfOutlines *outlines, PoDoFo::PdfMemDocument &pdf, const QJsonArray &entries)
{
    PoDoFo::PdfOutlineItem *previous = lastPdfOutlineSibling(parent ? parent->First() : outlines->First());
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        const int page = pageFromPdfLocation(entry.value(QStringLiteral("href")));
        const QString title = entry.value(QStringLiteral("label")).toString().trimmed();
        if (page < 0 || static_cast<unsigned>(page) >= pdf.GetPages().GetCount() || title.isEmpty()) {
            continue;
        }

        PoDoFo::PdfOutlineItem *item = nullptr;
        if (previous && previous->GetParentOutline()) {
            item = &previous->CreateNext(pdfStringFromQString(title));
        } else if (parent) {
            item = &parent->CreateChild(pdfStringFromQString(title));
        } else {
            // PoDoFo 1.1.1 doesn't retain the in-memory parent pointer of a
            // newly-created outline item. Creating a sibling through the
            // parent keeps the First/Last links valid in the saved PDF.
            item = &outlines->CreateChild(pdfStringFromQString(title));
        }

        auto destination = pdf.CreateDestination();
        destination->SetDestination(pdf.GetPages().GetPageAt(static_cast<unsigned>(page)));
        item->SetDestination(*destination);

        appendPdfOutlineItems(item, outlines, pdf, entry.value(QStringLiteral("subitems")).toArray());
        previous = item;
    }
}

QString pdfLocationForViewport(const Okular::DocumentViewport &viewport)
{
    QJsonObject location;
    location.insert(QStringLiteral("format"), QStringLiteral("pdf"));
    location.insert(QStringLiteral("page"), viewport.pageNumber);
    location.insert(QStringLiteral("pageNumber"), viewport.pageNumber + 1);
    if (viewport.rePos.enabled) {
        location.insert(QStringLiteral("x"), viewport.rePos.normalizedX);
        location.insert(QStringLiteral("y"), viewport.rePos.normalizedY);
        location.insert(QStringLiteral("position"),
                        viewport.rePos.pos == Okular::DocumentViewport::TopLeft ? QStringLiteral("topLeft") : QStringLiteral("center"));
    }
    return QString::fromUtf8(QJsonDocument(location).toJson(QJsonDocument::Compact));
}

QString pdfLocationForPage(int page)
{
    return pdfLocationForViewport(Okular::DocumentViewport(page));
}

Okular::DocumentViewport viewportForPagePosition(int page, double normalizedX, double normalizedY, Okular::DocumentViewport::Position position)
{
    Okular::DocumentViewport viewport(page);
    if (std::isfinite(normalizedX) && std::isfinite(normalizedY)) {
        viewport.rePos.enabled = true;
        viewport.rePos.normalizedX = std::clamp(normalizedX, 0.0, 1.0);
        viewport.rePos.normalizedY = std::clamp(normalizedY, 0.0, 1.0);
        viewport.rePos.pos = position;
    }
    return viewport;
}

QString attributeValue(const QDomElement &element, const QString &firstName, const QString &secondName)
{
    if (element.hasAttribute(firstName)) {
        return element.attribute(firstName);
    }
    return element.attribute(secondName);
}

QString normalizedLine(const QString &line)
{
    return line.simplified();
}

int estimatedFullLineLength(const QStringList &lines)
{
    QVector<int> lineLengths;
    lineLengths.reserve(lines.size());

    for (const QString &line : lines) {
        const int length = normalizedLine(line).size();
        if (length > 0) {
            lineLengths.append(length);
        }
    }

    if (lineLengths.isEmpty()) {
        return 0;
    }

    std::sort(lineLengths.begin(), lineLengths.end());
    return lineLengths.at(((lineLengths.size() - 1) * 3) / 4);
}

bool isLikelyParagraphEndingLine(const QString &line, int fullLineLength)
{
    if (line.isEmpty() || fullLineLength < 20) {
        return false;
    }

    return line.size() * 5 <= fullLineLength * 4;
}

bool shouldRemoveHyphenAtLineBreak(const QString &line, const QString &nextLine)
{
    if (line.isEmpty() || nextLine.isEmpty()) {
        return false;
    }

    const QChar last = line.back();
    return (last == QLatin1Char('-') || last == QChar(0x00ad)) && nextLine.front().isLetterOrNumber();
}

QString textForTranslation(const QString &text)
{
    if (text.isEmpty()) {
        return text;
    }

    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    const QStringList lines = normalized.split(QLatin1Char('\n'));
    const int fullLineLength = estimatedFullLineLength(lines);
    QString result;

    for (int index = 0; index < lines.size();) {
        const QString currentLine = normalizedLine(lines.at(index));
        if (currentLine.isEmpty()) {
            ++index;
            continue;
        }

        result += currentLine;

        int nextIndex = index + 1;
        while (nextIndex < lines.size() && normalizedLine(lines.at(nextIndex)).isEmpty()) {
            ++nextIndex;
        }

        if (nextIndex >= lines.size()) {
            break;
        }

        const QString nextLine = normalizedLine(lines.at(nextIndex));
        if (nextIndex > index + 1) {
            result += QStringLiteral("\n\n");
        } else if (isLikelyParagraphEndingLine(currentLine, fullLineLength)) {
            result += QLatin1Char('\n');
        } else if (shouldRemoveHyphenAtLineBreak(currentLine, nextLine)) {
            result.chop(1);
        } else {
            result += QLatin1Char(' ');
        }

        index = nextIndex;
    }

    return result.trimmed();
}

Okular::DocumentViewport viewportForSynopsisElement(Okular::Document *document, const QDomElement &element)
{
    QString viewportString = attributeValue(element, QStringLiteral("Viewport"), QStringLiteral("Destination"));
    if (viewportString.isEmpty()) {
        const QString viewportName = attributeValue(element, QStringLiteral("ViewportName"), QStringLiteral("DestinationName"));
        if (!viewportName.isEmpty()) {
            viewportString = document->metaData(QStringLiteral("NamedViewport"), viewportName).toString();
        }
    }

    return viewportString.isEmpty() ? Okular::DocumentViewport() : Okular::DocumentViewport(viewportString);
}

QJsonArray synopsisChildrenToJson(Okular::Document *document, const QDomNode &parentNode, int pageCount, int &idCounter)
{
    QJsonArray entries;
    for (QDomNode node = parentNode.firstChild(); !node.isNull(); node = node.nextSibling()) {
        const QDomElement element = node.toElement();
        if (element.isNull()) {
            continue;
        }

        const QJsonArray subitems = synopsisChildrenToJson(document, element, pageCount, idCounter);
        const Okular::DocumentViewport viewport = viewportForSynopsisElement(document, element);

        QJsonObject entry;
        const QString label = element.tagName();
        entry.insert(QStringLiteral("label"), label.isEmpty() ? QStringLiteral(" ") : label);
        entry.insert(QStringLiteral("id"), QStringLiteral("pdf-toc-%1").arg(++idCounter));
        entry.insert(QStringLiteral("href"), viewport.isValid() && viewport.pageNumber < pageCount ? pdfLocationForPage(viewport.pageNumber) : QString());
        entry.insert(QStringLiteral("subitems"), subitems);
        entries.append(entry);
    }
    return entries;
}
}

OkularPdfDocumentObserver::OkularPdfDocumentObserver(Okular::Document *document)
    : m_document(document)
{
    if (m_document) {
        m_document->addObserver(this);
    }
}

OkularPdfDocumentObserver::~OkularPdfDocumentObserver()
{
    if (m_document) {
        m_document->removeObserver(this);
    }
}

void OkularPdfDocumentObserver::notifyPageChanged(int page, int flags)
{
    Q_EMIT pageChanged(page, flags);
}

OkularPdfDocument::OkularPdfDocument(QObject *parent)
    : QObject(parent)
{
    Arianna::initializeOkularPdfSupport();

    m_document = std::make_unique<Okular::Document>(nullptr);
    m_pageviewObserver = std::make_unique<OkularPdfDocumentObserver>(m_document.get());

    connect(m_pageviewObserver.get(), &OkularPdfDocumentObserver::pageChanged, this, &OkularPdfDocument::pageChanged);
    connect(m_document.get(), &Okular::Document::error, this, [this](const QString &text, int /*duration*/) {
        if (!text.isEmpty()) {
            setErrorString(text);
        }
    });
}

OkularPdfDocument::~OkularPdfDocument() = default;

QUrl OkularPdfDocument::source() const
{
    return m_source;
}

void OkularPdfDocument::setSource(const QUrl &source)
{
    if (m_source == source) {
        return;
    }

    m_source = source;
    Q_EMIT sourceChanged();
    openSource();
}

OkularPdfDocument::Status OkularPdfDocument::status() const
{
    return m_status;
}

bool OkularPdfDocument::opened() const
{
    return m_status == Ready;
}

bool OkularPdfDocument::loading() const
{
    return m_status == Loading;
}

bool OkularPdfDocument::needsPassword() const
{
    return m_needsPassword;
}

int OkularPdfDocument::pageCount() const
{
    return m_pageCount;
}

int OkularPdfDocument::currentPage() const
{
    return m_currentPage;
}

void OkularPdfDocument::setCurrentPage(int page)
{
    if (m_pageCount > 0) {
        page = std::max(0, std::min(page, m_pageCount - 1));
    } else {
        page = 0;
    }

    if (m_document->isOpened()) {
        m_document->setViewportPage(page);
    }
    setCurrentPageValue(page);
}

QString OkularPdfDocument::password() const
{
    return m_password;
}

void OkularPdfDocument::setPassword(const QString &password)
{
    if (m_password == password) {
        return;
    }

    m_password = password;
    Q_EMIT passwordChanged();
    if (!m_source.isEmpty()) {
        openSource(m_password);
    }
}

QString OkularPdfDocument::errorString() const
{
    return m_errorString;
}

QString OkularPdfDocument::title() const
{
    return m_title;
}

QString OkularPdfDocument::author() const
{
    return m_author;
}

QString OkularPdfDocument::subject() const
{
    return m_subject;
}

QString OkularPdfDocument::keywords() const
{
    return m_keywords;
}

QString OkularPdfDocument::creator() const
{
    return m_creator;
}

QString OkularPdfDocument::producer() const
{
    return m_producer;
}

QString OkularPdfDocument::creationDate() const
{
    return m_creationDate;
}

QString OkularPdfDocument::selectedText() const
{
    if (!m_document->isOpened() || m_pagesWithTextSelection.isEmpty()) {
        return QString();
    }

    QString text;
    QList<int> selectedPages = m_pagesWithTextSelection.values();
    std::sort(selectedPages.begin(), selectedPages.end());

    if (selectedPages.size() == 1) {
        const int page = selectedPages.constFirst();
        const Okular::Page *okularPage = m_document->page(page);
        if (okularPage) {
            if (!okularPage->hasTextPage()) {
                m_document->requestTextPage(page);
            }
            text.append(okularPage->text(okularPage->textSelection(), Okular::TextPage::CentralPixelTextAreaInclusionBehaviour));
        }
    } else {
        const int firstPage = selectedPages.constFirst();
        const Okular::Page *firstOkularPage = m_document->page(firstPage);
        if (firstOkularPage) {
            if (!firstOkularPage->hasTextPage()) {
                m_document->requestTextPage(firstPage);
            }
            text.append(firstOkularPage->text(firstOkularPage->textSelection(), Okular::TextPage::CentralPixelTextAreaInclusionBehaviour));
        }

        const int end = selectedPages.size() - 1;
        for (int index = 1; index < end; ++index) {
            const int page = selectedPages.at(index);
            const Okular::Page *okularPage = m_document->page(page);
            if (!okularPage) {
                continue;
            }
            if (!okularPage->hasTextPage()) {
                m_document->requestTextPage(page);
            }
            text.append(okularPage->text(nullptr, Okular::TextPage::CentralPixelTextAreaInclusionBehaviour));
        }

        const int lastPage = selectedPages.constLast();
        const Okular::Page *lastOkularPage = m_document->page(lastPage);
        if (lastOkularPage) {
            if (!lastOkularPage->hasTextPage()) {
                m_document->requestTextPage(lastPage);
            }
            text.append(lastOkularPage->text(lastOkularPage->textSelection(), Okular::TextPage::CentralPixelTextAreaInclusionBehaviour));
        }
    }

    if (text.endsWith(QLatin1Char('\n'))) {
        text.chop(1);
    }
    return text;
}

QString OkularPdfDocument::selectedTextForTranslation() const
{
    return textForTranslation(selectedText());
}

QVariantMap OkularPdfDocument::writeTableOfContentsToPdf(const QString &tableOfContentsJson, const QString &targetFileName)
{
    QVariantMap result;
    const QString inputFileName =
        !targetFileName.trimmed().isEmpty() ? QFileInfo(targetFileName).absoluteFilePath() : (m_source.isLocalFile() ? m_source.toLocalFile() : QString());
    qDebug() << "PDF TOC write requested:" << inputFileName;
    if (inputFileName.isEmpty() || !QFileInfo(inputFileName).isFile()) {
        qWarning() << "PDF TOC write source is not a local file:" << inputFileName;
        result.insert(QStringLiteral("message"), i18n("Only local PDF files can be modified."));
        return result;
    }

    const QJsonDocument tableOfContentsDocument = QJsonDocument::fromJson(tableOfContentsJson.toUtf8());
    if (!tableOfContentsDocument.isArray()) {
        result.insert(QStringLiteral("message"), i18n("The table of contents is invalid."));
        return result;
    }

    QTemporaryFile outputFile(QFileInfo(inputFileName).dir().filePath(QStringLiteral(".arianna-toc-XXXXXX.pdf")));
    outputFile.setAutoRemove(false);
    if (!outputFile.open()) {
        result.insert(QStringLiteral("message"), i18n("Unable to create a temporary PDF file."));
        return result;
    }
    const QString outputFileName = outputFile.fileName();
    outputFile.close();
    QFile::remove(outputFileName);
    if (!QFile::copy(inputFileName, outputFileName)) {
        qWarning() << "PDF TOC temporary copy failed:" << inputFileName << outputFileName;
        result.insert(QStringLiteral("message"), i18n("Unable to copy the original PDF to a temporary file."));
        return result;
    }
    qDebug() << "PDF TOC temporary copy:" << outputFileName << QFileInfo(outputFileName).size() << "bytes";

    try {
        const std::string outputPath = outputFileName.toStdString();
        const auto inputOutput = std::make_shared<PoDoFo::FileStreamDevice>(outputPath, PoDoFo::FileMode::Open);
        PoDoFo::PdfMemDocument pdf;
        pdf.Load(inputOutput);
        PoDoFo::PdfOutlines &outlines = pdf.GetOrCreateOutlines();
        appendPdfOutlineItems(nullptr, &outlines, pdf, tableOfContentsDocument.array());
        pdf.SaveUpdate(*inputOutput, PoDoFo::PdfSaveOptions::NoMetadataUpdate);
        qDebug() << "PDF TOC PoDoFo update written:" << outputFileName << QFileInfo(outputFileName).size() << "bytes";
    } catch (const std::exception &error) {
        qWarning() << "PDF TOC PoDoFo write failed:" << error.what();
        QFile::remove(outputFileName);
        result.insert(QStringLiteral("message"), i18n("Unable to write the PDF table of contents: %1", QString::fromLocal8Bit(error.what())));
        return result;
    }

    const QString backupFileName = inputFileName + QStringLiteral(".arianna-toc-backup");
    QFile::remove(backupFileName);
    const bool backupCreated = QFile::rename(inputFileName, backupFileName);
    const bool originalReplaced = backupCreated && QFile::rename(outputFileName, inputFileName);
    qDebug() << "PDF TOC replacement:" << "backup=" << backupCreated << "replaced=" << originalReplaced << "new size=" << QFileInfo(inputFileName).size();
    if (!originalReplaced) {
        QFile::remove(outputFileName);
        QFile::rename(backupFileName, inputFileName);
        result.insert(QStringLiteral("message"), i18n("Unable to replace the original PDF."));
        return result;
    }
    QFile::remove(backupFileName);
    result.insert(QStringLiteral("success"), true);
    result.insert(QStringLiteral("message"), i18n("The table of contents was written to the PDF."));
    return result;
}

void OkularPdfDocument::reload()
{
    openSource(m_password);
}

bool OkularPdfDocument::hasTextSelection() const
{
    return !m_pagesWithTextSelection.isEmpty();
}

QString OkularPdfDocument::tableOfContentsJson() const
{
    return m_tableOfContentsJson;
}

QSizeF OkularPdfDocument::pagePointSize(int page) const
{
    if (!m_document->isOpened() || page < 0 || page >= m_pageCount) {
        return QSizeF(1, 1);
    }

    const Okular::Page *okularPage = m_document->page(page);
    return okularPage ? QSizeF(okularPage->width(), okularPage->height()) : QSizeF(1, 1);
}

Okular::Document *OkularPdfDocument::document() const
{
    return m_document.get();
}

OkularPdfDocumentObserver *OkularPdfDocument::pageviewObserver() const
{
    return m_pageviewObserver.get();
}

QString OkularPdfDocument::internalLocationForAction(const Okular::Action *action) const
{
    if (!action || !m_document->isOpened() || m_pageCount <= 0) {
        return QString();
    }

    Okular::DocumentViewport targetViewport;
    if (action->actionType() == Okular::Action::Goto) {
        const auto *gotoAction = static_cast<const Okular::GotoAction *>(action);
        if (gotoAction->isExternal()) {
            return QString();
        }

        targetViewport = gotoAction->destViewport();
        if (!targetViewport.isValid() && !gotoAction->destinationName().isEmpty()) {
            targetViewport = Okular::DocumentViewport(m_document->metaData(QStringLiteral("NamedViewport"), gotoAction->destinationName()).toString());
        }
    } else if (action->actionType() == Okular::Action::DocAction) {
        const auto *documentAction = static_cast<const Okular::DocumentAction *>(action);
        switch (documentAction->documentActionType()) {
        case Okular::DocumentAction::PageFirst:
            targetViewport = Okular::DocumentViewport(0);
            break;
        case Okular::DocumentAction::PagePrev:
            targetViewport = Okular::DocumentViewport(std::max(0, m_currentPage - 1));
            break;
        case Okular::DocumentAction::PageNext:
            targetViewport = Okular::DocumentViewport(std::min(m_pageCount - 1, m_currentPage + 1));
            break;
        case Okular::DocumentAction::PageLast:
            targetViewport = Okular::DocumentViewport(m_pageCount - 1);
            break;
        default:
            break;
        }
    }

    return targetViewport.isValid() && targetViewport.pageNumber < m_pageCount ? pdfLocationForViewport(targetViewport) : QString();
}

QString OkularPdfDocument::locationForPagePosition(int page, double normalizedX, double normalizedY, Okular::DocumentViewport::Position position) const
{
    if (!m_document->isOpened() || page < 0 || page >= m_pageCount) {
        return QString();
    }

    return pdfLocationForViewport(viewportForPagePosition(page, normalizedX, normalizedY, position));
}

void OkularPdfDocument::clearTextSelection()
{
    if (m_pagesWithTextSelection.isEmpty()) {
        return;
    }

    const QSet<int> selectedPages = m_pagesWithTextSelection;
    m_pagesWithTextSelection.clear();
    if (m_document->isOpened()) {
        for (const int page : selectedPages) {
            m_document->setPageTextSelection(page, nullptr, QColor());
        }
    }
    Q_EMIT selectedTextChanged();
}

void OkularPdfDocument::copySelectedText(bool removeLineBreaks) const
{
    QString text = selectedText();
    if (text.isEmpty()) {
        return;
    }

    if (removeLineBreaks) {
        text = Okular::removeLineBreaks(text);
    }

    if (QClipboard *clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(text, QClipboard::Clipboard);
    }
}

void OkularPdfDocument::setTextSelection(int page, std::unique_ptr<Okular::RegularAreaRect> &&rect)
{
    if (!m_document->isOpened() || page < 0 || page >= m_pageCount) {
        return;
    }

    if (!rect || rect->isNull()) {
        if (!m_pagesWithTextSelection.contains(page)) {
            return;
        }

        m_pagesWithTextSelection.remove(page);
        m_document->setPageTextSelection(page, nullptr, QColor());
        Q_EMIT selectedTextChanged();
        return;
    }

    QColor selectionColor = QGuiApplication::palette().color(QPalette::Active, QPalette::Highlight);
    selectionColor.setAlpha(96);
    m_document->setPageTextSelection(page, std::move(rect), selectionColor);
    m_pagesWithTextSelection.insert(page);
    Q_EMIT selectedTextChanged();
}

void OkularPdfDocument::openSource(const QString &password)
{
    m_document->closeDocument();
    clearDocumentState();

    if (m_source.isEmpty()) {
        setStatus(Null);
        return;
    }

    setStatus(Loading);

    QUrl realUrl = m_source;
    const QString fileName = realUrl.isLocalFile() ? realUrl.toLocalFile() : realUrl.toString(QUrl::PreferLocalFile);
    if (fileName.isEmpty()) {
        setErrorString(i18n("No PDF file was specified"));
        setStatus(Error);
        return;
    }

    const Okular::Document::OpenResult result = Arianna::openOkularPdfDocument(*m_document, fileName, realUrl, password);
    setNeedsPassword(result == Okular::Document::OpenNeedsPassword);

    if (result == Okular::Document::OpenNeedsPassword) {
        setErrorString(i18n("Password Required"));
        setStatus(NeedsPassword);
        Q_EMIT passwordRequired();
        return;
    }

    if (result != Okular::Document::OpenSuccess || !m_document->isOpened()) {
        if (m_errorString.isEmpty()) {
            setErrorString(i18n("Unable to open PDF"));
        }
        setStatus(Error);
        return;
    }

    setErrorString(QString());
    setPageCount(static_cast<int>(m_document->pages()));
    updateMetadata();
    updateTableOfContents();
    setCurrentPageValue(std::max(0, std::min(static_cast<int>(m_document->currentPage()), std::max(0, m_pageCount - 1))));
    setStatus(Ready);
}

void OkularPdfDocument::clearDocumentState()
{
    const bool hadTextSelection = !m_pagesWithTextSelection.isEmpty();
    m_pagesWithTextSelection.clear();
    const bool hadTableOfContents = m_tableOfContentsJson != QLatin1String("[]");
    m_tableOfContentsJson = QStringLiteral("[]");

    setPageCount(0);
    setCurrentPageValue(0);
    setNeedsPassword(false);
    setErrorString(QString());

    const bool hadMetadata = !m_title.isEmpty() || !m_author.isEmpty() || !m_subject.isEmpty() || !m_keywords.isEmpty() || !m_creator.isEmpty()
        || !m_producer.isEmpty() || !m_creationDate.isEmpty();
    m_title.clear();
    m_author.clear();
    m_subject.clear();
    m_keywords.clear();
    m_creator.clear();
    m_producer.clear();
    m_creationDate.clear();
    if (hadMetadata) {
        Q_EMIT metadataChanged();
    }
    if (hadTextSelection) {
        Q_EMIT selectedTextChanged();
    }
    if (hadTableOfContents) {
        Q_EMIT tableOfContentsChanged();
    }
}

void OkularPdfDocument::updateMetadata()
{
    const Okular::DocumentInfo info = m_document->documentInfo();
    const QString title = info.get(Okular::DocumentInfo::Title);
    const QString author = info.get(Okular::DocumentInfo::Author);
    const QString subject = info.get(Okular::DocumentInfo::Subject);
    const QString keywords = info.get(Okular::DocumentInfo::Keywords);
    const QString creator = info.get(Okular::DocumentInfo::Creator);
    const QString producer = info.get(Okular::DocumentInfo::Producer);
    const QString creationDate = info.get(Okular::DocumentInfo::CreationDate);

    const bool changed = m_title != title || m_author != author || m_subject != subject || m_keywords != keywords || m_creator != creator
        || m_producer != producer || m_creationDate != creationDate;

    m_title = title;
    m_author = author;
    m_subject = subject;
    m_keywords = keywords;
    m_creator = creator;
    m_producer = producer;
    m_creationDate = creationDate;

    if (changed) {
        Q_EMIT metadataChanged();
    }
}

void OkularPdfDocument::updateTableOfContents()
{
    QString tableOfContentsJson = QStringLiteral("[]");
    const Okular::DocumentSynopsis *synopsis = m_document->documentSynopsis();
    if (synopsis) {
        int idCounter = 0;
        tableOfContentsJson =
            QString::fromUtf8(QJsonDocument(synopsisChildrenToJson(m_document.get(), *synopsis, m_pageCount, idCounter)).toJson(QJsonDocument::Compact));
    }

    if (m_tableOfContentsJson == tableOfContentsJson) {
        return;
    }

    m_tableOfContentsJson = tableOfContentsJson;
    Q_EMIT tableOfContentsChanged();
}

void OkularPdfDocument::setStatus(Status status)
{
    if (m_status == status) {
        return;
    }

    const bool wasOpened = opened();
    const bool wasLoading = loading();
    m_status = status;
    Q_EMIT statusChanged();
    if (wasOpened != opened()) {
        Q_EMIT openedChanged();
    }
    if (wasLoading != loading()) {
        Q_EMIT loadingChanged();
    }
}

void OkularPdfDocument::setPageCount(int pageCount)
{
    pageCount = std::max(0, pageCount);
    if (m_pageCount == pageCount) {
        return;
    }

    m_pageCount = pageCount;
    Q_EMIT pageCountChanged();
}

void OkularPdfDocument::setNeedsPassword(bool needsPassword)
{
    if (m_needsPassword == needsPassword) {
        return;
    }

    m_needsPassword = needsPassword;
    Q_EMIT needsPasswordChanged();
}

void OkularPdfDocument::setErrorString(const QString &errorString)
{
    if (m_errorString == errorString) {
        return;
    }

    m_errorString = errorString;
    Q_EMIT errorChanged();
}

void OkularPdfDocument::setCurrentPageValue(int page)
{
    if (m_currentPage == page) {
        return;
    }

    m_currentPage = page;
    Q_EMIT currentPageChanged();
}
