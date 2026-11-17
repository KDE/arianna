// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

#include "okularpdfdocument.h"

#include "okularpdfsupport.h"
#include "pdfreferenceindex.h"

#include <QClipboard>
#include <QDebug>
#include <QDir>
#include <QDomElement>
#include <QDomNode>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QStringList>
#include <QTemporaryFile>
#include <QTextBoundaryFinder>
#include <QUuid>
#include <QVector>

#include <KLocalizedString>

#include <algorithm>
#include <cmath>
#include <functional>

#include <core/action.h>
#include <core/area.h>
#include <core/misc.h>
#include <core/page.h>
#include <core/textpage.h>

#include <podofo/auxiliary/StreamDevice.h>
#include <podofo/main/PdfAnnotation_Types.h>
#include <podofo/main/PdfArray.h>
#include <podofo/main/PdfDestination.h>
#include <podofo/main/PdfMemDocument.h>
#include <podofo/main/PdfOutlines.h>
#include <podofo/main/PdfPage.h>

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

PoDoFo::PdfOutlineItem *appendPdfOutlineItem(PoDoFo::PdfOutlineItem *parent, PoDoFo::PdfOutlines &outlines, const QString &title)
{
    PoDoFo::PdfOutlineItem *previous = lastPdfOutlineSibling(parent ? parent->First() : outlines.First());
    if (previous) {
        return &previous->CreateNext(pdfStringFromQString(title));
    }

    return parent ? &parent->CreateChild(pdfStringFromQString(title)) : &outlines.CreateChild(pdfStringFromQString(title));
}

void setPdfOutlineDestination(PoDoFo::PdfOutlineItem &item, PoDoFo::PdfMemDocument &pdf, int page)
{
    auto destination = pdf.CreateDestination();
    destination->SetDestination(pdf.GetPages().GetPageAt(static_cast<unsigned>(page)));
    item.SetDestination(*destination);
}

void appendPdfOutlineItems(PoDoFo::PdfOutlineItem *parent, PoDoFo::PdfOutlines *outlines, PoDoFo::PdfMemDocument &pdf, const QJsonArray &entries)
{
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        const int page = pageFromPdfLocation(entry.value(QStringLiteral("href")));
        const QString title = entry.value(QStringLiteral("label")).toString().trimmed();
        if (page < 0 || static_cast<unsigned>(page) >= pdf.GetPages().GetCount() || title.isEmpty()) {
            continue;
        }

        PoDoFo::PdfOutlineItem *item = appendPdfOutlineItem(parent, *outlines, title);
        setPdfOutlineDestination(*item, pdf, page);

        appendPdfOutlineItems(item, outlines, pdf, entry.value(QStringLiteral("subitems")).toArray());
    }
}

struct PdfOutlineStructure {
    QHash<QString, PoDoFo::PdfOutlineItem *> itemsById;
    QHash<PoDoFo::PdfOutlineItem *, PoDoFo::PdfOutlineItem *> parents;
    QHash<PoDoFo::PdfOutlineItem *, QVector<PoDoFo::PdfOutlineItem *>> children;
};

void collectPdfOutlineStructure(PoDoFo::PdfOutlineItem *first, PoDoFo::PdfOutlineItem *parent, PdfOutlineStructure &structure, int &idCounter)
{
    QVector<PoDoFo::PdfOutlineItem *> children;
    for (PoDoFo::PdfOutlineItem *item = first; item; item = item->Next()) {
        children.append(item);
        structure.parents.insert(item, parent);
        collectPdfOutlineStructure(item->First(), item, structure, idCounter);
        structure.itemsById.insert(QStringLiteral("pdf-toc-%1").arg(++idCounter), item);
    }
    structure.children.insert(parent, children);
}

void setPdfOutlineReference(PoDoFo::PdfDictionary &dictionary, const char *name, PoDoFo::PdfOutlineItem *item)
{
    if (item) {
        dictionary.AddKeyIndirect(PoDoFo::PdfName(name), item->GetObject());
    } else {
        dictionary.RemoveKey(name);
    }
}

void updatePdfOutlineSiblingLinks(PoDoFo::PdfOutlineItem *parent, PoDoFo::PdfOutlines &outlines, const QVector<PoDoFo::PdfOutlineItem *> &items)
{
    PoDoFo::PdfDictionary &parentDictionary = parent ? parent->GetDictionary() : outlines.GetDictionary();
    if (items.isEmpty()) {
        parentDictionary.RemoveKey("First");
        parentDictionary.RemoveKey("Last");
        return;
    }

    parentDictionary.AddKeyIndirect(PoDoFo::PdfName("First"), items.first()->GetObject());
    parentDictionary.AddKeyIndirect(PoDoFo::PdfName("Last"), items.last()->GetObject());
    for (qsizetype index = 0; index < items.size(); ++index) {
        PoDoFo::PdfOutlineItem *item = items.at(index);
        PoDoFo::PdfDictionary &dictionary = item->GetDictionary();
        dictionary.AddKeyIndirect(PoDoFo::PdfName("Parent"), parent ? parent->GetObject() : outlines.GetObject());
        setPdfOutlineReference(dictionary, "Prev", index > 0 ? items.at(index - 1) : nullptr);
        setPdfOutlineReference(dictionary, "Next", index + 1 < items.size() ? items.at(index + 1) : nullptr);
    }
}

void markPdfOutlineParentAsModified(QVector<PoDoFo::PdfOutlineItem *> &parents, PoDoFo::PdfOutlineItem *parent)
{
    if (!parents.contains(parent)) {
        parents.append(parent);
    }
}

bool insertPdfOutlineItems(PoDoFo::PdfOutlines &outlines, PoDoFo::PdfMemDocument &pdf, const QJsonArray &entries, QString &errorMessage)
{
    PdfOutlineStructure structure;
    int idCounter = 0;
    collectPdfOutlineStructure(outlines.First(), nullptr, structure, idCounter);

    QHash<PoDoFo::PdfOutlineItem *, PoDoFo::PdfOutlineItem *> lastItemsInsertedBefore;
    QHash<PoDoFo::PdfOutlineItem *, PoDoFo::PdfOutlineItem *> lastItemsInsertedAfter;
    QVector<PoDoFo::PdfOutlineItem *> modifiedParents;

    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        const int page = pageFromPdfLocation(entry.value(QStringLiteral("href")));
        const QString title = entry.value(QStringLiteral("label")).toString().trimmed();
        if (page < 0 || static_cast<unsigned>(page) >= pdf.GetPages().GetCount() || title.isEmpty()) {
            continue;
        }

        const QString targetId = entry.value(QStringLiteral("targetId")).toString();
        const QString placement = entry.value(QStringLiteral("placement")).toString();
        PoDoFo::PdfOutlineItem *parent = nullptr;
        PoDoFo::PdfOutlineItem *item = nullptr;

        if (targetId.isEmpty() || placement.isEmpty() || placement == QStringLiteral("append")) {
            item = appendPdfOutlineItem(nullptr, outlines, title);
            structure.children[nullptr].append(item);
            markPdfOutlineParentAsModified(modifiedParents, nullptr);
        } else {
            const auto targetIt = structure.itemsById.constFind(targetId);
            if (targetIt == structure.itemsById.constEnd()) {
                errorMessage = i18n("The selected table of contents item is no longer available.");
                return false;
            }

            PoDoFo::PdfOutlineItem *target = targetIt.value();
            if (placement == QStringLiteral("child")) {
                parent = target;
                item = appendPdfOutlineItem(parent, outlines, title);
                structure.children[parent].append(item);
            } else {
                parent = structure.parents.value(target, nullptr);
                item = &target->CreateNext(pdfStringFromQString(title));
                QVector<PoDoFo::PdfOutlineItem *> &siblings = structure.children[parent];
                const int targetIndex = siblings.indexOf(target);
                if (targetIndex < 0) {
                    errorMessage = i18n("The selected table of contents item is no longer available.");
                    return false;
                }

                if (placement == QStringLiteral("before")) {
                    PoDoFo::PdfOutlineItem *previous = lastItemsInsertedBefore.value(target, nullptr);
                    const int insertionIndex = previous ? siblings.indexOf(previous) + 1 : targetIndex;
                    siblings.insert(insertionIndex, item);
                    lastItemsInsertedBefore.insert(target, item);
                } else if (placement == QStringLiteral("after")) {
                    PoDoFo::PdfOutlineItem *previous = lastItemsInsertedAfter.value(target, target);
                    siblings.insert(siblings.indexOf(previous) + 1, item);
                    lastItemsInsertedAfter.insert(target, item);
                } else {
                    errorMessage = i18n("The selected table of contents position is invalid.");
                    return false;
                }
            }
            markPdfOutlineParentAsModified(modifiedParents, parent);
        }

        setPdfOutlineDestination(*item, pdf, page);
        appendPdfOutlineItems(item, &outlines, pdf, entry.value(QStringLiteral("subitems")).toArray());
    }

    for (PoDoFo::PdfOutlineItem *parent : modifiedParents) {
        updatePdfOutlineSiblingLinks(parent, outlines, structure.children.value(parent));
    }

    return true;
}

using PdfOutlineMutation = std::function<bool(PoDoFo::PdfOutlines &, PoDoFo::PdfMemDocument &, QString &)>;

QVariantMap mutatePdfOutline(const QString &inputFileName, const PdfOutlineMutation &mutation, const QString &successMessage)
{
    QVariantMap result;
    qDebug() << "PDF TOC write requested:" << inputFileName;
    if (inputFileName.isEmpty() || !QFileInfo(inputFileName).isFile()) {
        qWarning() << "PDF TOC write source is not a local file:" << inputFileName;
        result.insert(QStringLiteral("message"), i18n("Only local PDF files can be modified."));
        return result;
    }

    QTemporaryFile outputFile(QFileInfo(inputFileName).dir().filePath(QStringLiteral(".arianna-toc-XXXXXX.pdf")));
    outputFile.setAutoRemove(false);
    if (!outputFile.open()) {
        result.insert(QStringLiteral("message"), i18n("Unable to create a temporary PDF file."));
        return result;
    }
    const QString outputFileName = outputFile.fileName();
    const QString rewrittenFileName = outputFileName + QStringLiteral(".rewritten");
    outputFile.close();
    QFile::remove(outputFileName);
    if (!QFile::copy(inputFileName, outputFileName)) {
        qWarning() << "PDF TOC temporary copy failed:" << inputFileName << outputFileName;
        result.insert(QStringLiteral("message"), i18n("Unable to copy the original PDF to a temporary file."));
        return result;
    }

    try {
        const auto inputOutput = std::make_shared<PoDoFo::FileStreamDevice>(outputFileName.toStdString(), PoDoFo::FileMode::Open);
        PoDoFo::PdfMemDocument pdf;
        pdf.Load(inputOutput);
        PoDoFo::PdfOutlines &outlines = pdf.GetOrCreateOutlines();
        QString mutationError;
        if (!mutation(outlines, pdf, mutationError)) {
            QFile::remove(outputFileName);
            result.insert(QStringLiteral("message"), mutationError);
            return result;
        }
        pdf.Save(rewrittenFileName.toStdString(), PoDoFo::PdfSaveOptions::NoMetadataUpdate);
        inputOutput->Close();
        QFile::remove(outputFileName);
        if (!QFile::rename(rewrittenFileName, outputFileName)) {
            result.insert(QStringLiteral("message"), i18n("Unable to finalize the rewritten PDF."));
            return result;
        }
    } catch (const std::exception &error) {
        qWarning() << "PDF TOC PoDoFo write failed:" << error.what();
        QFile::remove(outputFileName);
        QFile::remove(rewrittenFileName);
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
    result.insert(QStringLiteral("message"), successMessage);
    return result;
}

bool findPdfOutlineItem(PoDoFo::PdfOutlines &outlines,
                        const QString &tocId,
                        PdfOutlineStructure &structure,
                        PoDoFo::PdfOutlineItem *&item,
                        QString &errorMessage)
{
    int idCounter = 0;
    collectPdfOutlineStructure(outlines.First(), nullptr, structure, idCounter);
    const auto itemIt = structure.itemsById.constFind(tocId);
    if (itemIt == structure.itemsById.constEnd()) {
        errorMessage = i18n("The selected table of contents item is no longer available.");
        return false;
    }

    item = itemIt.value();
    return true;
}

struct PdfNormalizedRectangle {
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;
};

struct PdfLinkSelection {
    int page = -1;
    QVector<PdfNormalizedRectangle> rectangles;
};

bool readNormalizedCoordinate(const QVariantMap &rectangle, const QString &name, double &value)
{
    bool ok = false;
    value = rectangle.value(name).toDouble(&ok);
    return ok && std::isfinite(value);
}

bool parsePdfLinkSelection(const QVariantMap &selection, PdfLinkSelection &parsed, QString &errorMessage)
{
    bool pageOk = false;
    parsed.page = selection.value(QStringLiteral("page")).toInt(&pageOk);
    const QVariantList rectangles = selection.value(QStringLiteral("rects")).toList();
    if (!pageOk || parsed.page < 0 || rectangles.isEmpty()) {
        errorMessage = i18n("The PDF text selection is incomplete.");
        return false;
    }

    for (const QVariant &rectangleValue : rectangles) {
        const QVariantMap rectangle = rectangleValue.toMap();
        double x = 0;
        double y = 0;
        double width = 0;
        double height = 0;
        if (!readNormalizedCoordinate(rectangle, QStringLiteral("x"), x) || !readNormalizedCoordinate(rectangle, QStringLiteral("y"), y)
            || !readNormalizedCoordinate(rectangle, QStringLiteral("width"), width)
            || !readNormalizedCoordinate(rectangle, QStringLiteral("height"), height)) {
            errorMessage = i18n("The PDF text selection contains invalid geometry.");
            return false;
        }

        const double left = std::clamp(std::min(x, x + width), 0.0, 1.0);
        const double top = std::clamp(std::min(y, y + height), 0.0, 1.0);
        const double right = std::clamp(std::max(x, x + width), 0.0, 1.0);
        const double bottom = std::clamp(std::max(y, y + height), 0.0, 1.0);
        if (right <= left || bottom <= top) {
            errorMessage = i18n("The PDF text selection contains an empty area.");
            return false;
        }

        parsed.rectangles.append({left, top, right, bottom});
    }

    return true;
}

QPointF pdfPointForNormalizedPoint(const PoDoFo::PdfPage &page, double x, double y)
{
    const PoDoFo::Rect cropBox = page.GetCropBox();
    double pageX = 0;
    double pageY = 0;
    switch (page.GetRotation()) {
    case 90:
        pageX = y;
        pageY = x;
        break;
    case 180:
        pageX = 1.0 - x;
        pageY = y;
        break;
    case 270:
        pageX = 1.0 - y;
        pageY = 1.0 - x;
        break;
    default:
        pageX = x;
        pageY = 1.0 - y;
        break;
    }

    return QPointF(cropBox.GetLeft() + pageX * cropBox.Width, cropBox.GetBottom() + pageY * cropBox.Height);
}

PoDoFo::Rect pdfRectangleForNormalizedRectangle(const PoDoFo::PdfPage &page, const PdfNormalizedRectangle &rectangle)
{
    const QPointF topLeft = pdfPointForNormalizedPoint(page, rectangle.left, rectangle.top);
    const QPointF bottomRight = pdfPointForNormalizedPoint(page, rectangle.right, rectangle.bottom);
    return PoDoFo::Rect::FromCorners(topLeft.x(), topLeft.y(), bottomRight.x(), bottomRight.y());
}

PoDoFo::PdfArray pdfQuadPointsForRectangles(const QVector<PoDoFo::Rect> &rectangles)
{
    PoDoFo::PdfArray quadPoints;
    for (const PoDoFo::Rect &rectangle : rectangles) {
        quadPoints.Add(PoDoFo::PdfObject(rectangle.GetLeft()));
        quadPoints.Add(PoDoFo::PdfObject(rectangle.GetBottom()));
        quadPoints.Add(PoDoFo::PdfObject(rectangle.GetRight()));
        quadPoints.Add(PoDoFo::PdfObject(rectangle.GetBottom()));
        quadPoints.Add(PoDoFo::PdfObject(rectangle.GetRight()));
        quadPoints.Add(PoDoFo::PdfObject(rectangle.GetTop()));
        quadPoints.Add(PoDoFo::PdfObject(rectangle.GetLeft()));
        quadPoints.Add(PoDoFo::PdfObject(rectangle.GetTop()));
    }
    return quadPoints;
}

void setAriannaAnchorDictionary(PoDoFo::PdfAnnotation &annotation, const QString &anchorId)
{
    PoDoFo::PdfDictionary &dictionary = annotation.GetDictionary();
    dictionary.AddKey(PoDoFo::PdfName("NM"), PoDoFo::PdfObject(pdfStringFromQString(anchorId)));
    dictionary.AddKey(PoDoFo::PdfName("AriannaAnchor"), PoDoFo::PdfObject(true));
}

QPointF normalizedPointForPdfPoint(const PoDoFo::PdfPage &page, const QPointF &point)
{
    const PoDoFo::Rect cropBox = page.GetCropBox();
    if (cropBox.Width <= 0 || cropBox.Height <= 0) {
        return {};
    }

    const double pageX = std::clamp((point.x() - cropBox.GetLeft()) / cropBox.Width, 0.0, 1.0);
    const double pageY = std::clamp((point.y() - cropBox.GetBottom()) / cropBox.Height, 0.0, 1.0);
    switch (page.GetRotation()) {
    case 90:
        return QPointF(pageY, pageX);
    case 180:
        return QPointF(1.0 - pageX, pageY);
    case 270:
        return QPointF(1.0 - pageY, 1.0 - pageX);
    default:
        return QPointF(pageX, 1.0 - pageY);
    }
}

QVariantMap normalizedRectangleMap(const QVector<QPointF> &points)
{
    if (points.isEmpty()) {
        return {};
    }

    double left = points.constFirst().x();
    double top = points.constFirst().y();
    double right = left;
    double bottom = top;
    for (const QPointF &point : points) {
        left = std::min(left, point.x());
        top = std::min(top, point.y());
        right = std::max(right, point.x());
        bottom = std::max(bottom, point.y());
    }

    if (right <= left || bottom <= top) {
        return {};
    }

    return {
        {QStringLiteral("x"), left},
        {QStringLiteral("y"), top},
        {QStringLiteral("width"), right - left},
        {QStringLiteral("height"), bottom - top},
    };
}

QVariantList normalizedRectanglesForAriannaAnchor(const PoDoFo::PdfPage &page, const PoDoFo::PdfAnnotation &annotation)
{
    QVariantList rectangles;
    if (annotation.GetType() == PoDoFo::PdfAnnotationType::Highlight) {
        const auto &highlight = static_cast<const PoDoFo::PdfAnnotationHighlight &>(annotation);
        const auto quadPoints = highlight.GetQuadPoints();
        if (quadPoints != nullptr) {
            for (unsigned offset = 0; offset + 7 < quadPoints->GetSize(); offset += 8) {
                QVector<QPointF> points;
                points.reserve(4);
                bool validQuad = true;
                for (unsigned pointIndex = 0; pointIndex < 4; ++pointIndex) {
                    double x = 0;
                    double y = 0;
                    validQuad = quadPoints->TryGetAtAs(offset + pointIndex * 2, x)
                        && quadPoints->TryGetAtAs(offset + pointIndex * 2 + 1, y);
                    if (!validQuad) {
                        break;
                    }
                    points.append(normalizedPointForPdfPoint(page, QPointF(x, y)));
                }

                const QVariantMap rectangle = validQuad ? normalizedRectangleMap(points) : QVariantMap();
                if (!rectangle.isEmpty()) {
                    rectangles.append(rectangle);
                }
            }
        }
    }

    if (!rectangles.isEmpty()) {
        return rectangles;
    }

    const PoDoFo::Rect bounds = annotation.GetRect();
    const QVariantMap boundsRectangle = normalizedRectangleMap({
        normalizedPointForPdfPoint(page, QPointF(bounds.GetLeft(), bounds.GetBottom())),
        normalizedPointForPdfPoint(page, QPointF(bounds.GetRight(), bounds.GetBottom())),
        normalizedPointForPdfPoint(page, QPointF(bounds.GetRight(), bounds.GetTop())),
        normalizedPointForPdfPoint(page, QPointF(bounds.GetLeft(), bounds.GetTop())),
    });
    if (!boundsRectangle.isEmpty()) {
        rectangles.append(boundsRectangle);
    }
    return rectangles;
}

QVariantMap writePdfAriannaAnchorAnnotation(const QString &inputFileName,
                                            const PdfLinkSelection &selection,
                                            const QString &anchorId,
                                            const QString &password)
{
    QVariantMap result;
    if (inputFileName.isEmpty() || !QFileInfo(inputFileName).isFile()) {
        result.insert(QStringLiteral("message"), i18n("Only local PDF files can be modified."));
        return result;
    }

    QTemporaryFile outputFile(QFileInfo(inputFileName).dir().filePath(QStringLiteral(".arianna-anchor-XXXXXX.pdf")));
    outputFile.setAutoRemove(false);
    if (!outputFile.open()) {
        result.insert(QStringLiteral("message"), i18n("Unable to create a temporary PDF file."));
        return result;
    }
    const QString outputFileName = outputFile.fileName();
    outputFile.close();
    QFile::remove(outputFileName);
    if (!QFile::copy(inputFileName, outputFileName)) {
        QFile::remove(outputFileName);
        result.insert(QStringLiteral("message"), i18n("Unable to copy the original PDF to a temporary file."));
        return result;
    }

    try {
        const auto inputOutput = std::make_shared<PoDoFo::FileStreamDevice>(outputFileName.toStdString(), PoDoFo::FileMode::Open);
        PoDoFo::PdfMemDocument pdf;
        const QByteArray utf8Password = password.toUtf8();
        pdf.Load(inputOutput, utf8Password.toStdString());
        if (pdf.IsEncrypted() && !pdf.HasOwnerPermissions() && !pdf.IsEditNotesAllowed()) {
            QFile::remove(outputFileName);
            result.insert(QStringLiteral("message"), i18n("The encrypted PDF does not allow adding or editing annotations."));
            return result;
        }
        if (static_cast<unsigned>(selection.page) >= pdf.GetPages().GetCount()) {
            QFile::remove(outputFileName);
            result.insert(QStringLiteral("message"), i18n("The PDF text selection is outside the document."));
            return result;
        }

        PoDoFo::PdfPage &page = pdf.GetPages().GetPageAt(static_cast<unsigned>(selection.page));
        QVector<PoDoFo::Rect> rectangles;
        rectangles.reserve(selection.rectangles.size());
        for (const PdfNormalizedRectangle &rectangle : selection.rectangles) {
            rectangles.append(pdfRectangleForNormalizedRectangle(page, rectangle));
        }
        if (rectangles.isEmpty()) {
            QFile::remove(outputFileName);
            result.insert(QStringLiteral("message"), i18n("The PDF anchor is empty."));
            return result;
        }

        PoDoFo::Rect bounds = rectangles.constFirst();
        for (qsizetype index = 1; index < rectangles.size(); ++index) {
            bounds = PoDoFo::Rect::FromCorners(std::min(bounds.GetLeft(), rectangles.at(index).GetLeft()),
                                               std::min(bounds.GetBottom(), rectangles.at(index).GetBottom()),
                                               std::max(bounds.GetRight(), rectangles.at(index).GetRight()),
                                               std::max(bounds.GetTop(), rectangles.at(index).GetTop()));
        }

        // Highlight annotations support QuadPoints, which keeps every selected
        // rectangle instead of reducing a multi-line selection to its bounds.
        PoDoFo::PdfAnnotationHighlight &anchor = page.GetAnnotations().CreateAnnot<PoDoFo::PdfAnnotationHighlight>(bounds);
        anchor.SetQuadPoints(pdfQuadPointsForRectangles(rectangles));
        anchor.SetBorderStyle(0, 0, 0);
        anchor.SetFlags(static_cast<PoDoFo::PdfAnnotationFlags>(static_cast<uint32_t>(PoDoFo::PdfAnnotationFlags::Hidden)
                                                                | static_cast<uint32_t>(PoDoFo::PdfAnnotationFlags::NoView)));
        setAriannaAnchorDictionary(anchor, anchorId);
        pdf.SaveUpdate(*inputOutput, PoDoFo::PdfSaveOptions::NoMetadataUpdate);
    } catch (const std::exception &error) {
        QFile::remove(outputFileName);
        result.insert(QStringLiteral("message"), i18n("Unable to write the PDF anchor: %1", QString::fromLocal8Bit(error.what())));
        return result;
    }

    const QString backupFileName = inputFileName + QStringLiteral(".arianna-anchor-backup");
    QFile::remove(backupFileName);
    const bool backupCreated = QFile::rename(inputFileName, backupFileName);
    const bool originalReplaced = backupCreated && QFile::rename(outputFileName, inputFileName);
    if (!originalReplaced) {
        QFile::remove(outputFileName);
        QFile::rename(backupFileName, inputFileName);
        result.insert(QStringLiteral("message"), i18n("Unable to replace the original PDF."));
        return result;
    }

    QFile::remove(backupFileName);
    result.insert(QStringLiteral("success"), true);
    result.insert(QStringLiteral("anchorId"), anchorId);
    result.insert(QStringLiteral("message"), i18n("The Arianna anchor was written to the PDF."));
    return result;
}

QVariantMap writePdfLinkAnnotation(const QString &inputFileName,
                                   const PdfLinkSelection &sourceSelection,
                                   const PdfLinkSelection &targetSelection,
                                   const QString &password)
{
    QVariantMap result;
    if (inputFileName.isEmpty() || !QFileInfo(inputFileName).isFile()) {
        result.insert(QStringLiteral("message"), i18n("Only local PDF files can be modified."));
        return result;
    }

    QTemporaryFile outputFile(QFileInfo(inputFileName).dir().filePath(QStringLiteral(".arianna-link-XXXXXX.pdf")));
    outputFile.setAutoRemove(false);
    if (!outputFile.open()) {
        result.insert(QStringLiteral("message"), i18n("Unable to create a temporary PDF file."));
        return result;
    }
    const QString outputFileName = outputFile.fileName();
    outputFile.close();
    QFile::remove(outputFileName);
    if (!QFile::copy(inputFileName, outputFileName)) {
        QFile::remove(outputFileName);
        result.insert(QStringLiteral("message"), i18n("Unable to copy the original PDF to a temporary file."));
        return result;
    }

    try {
        const auto inputOutput = std::make_shared<PoDoFo::FileStreamDevice>(outputFileName.toStdString(), PoDoFo::FileMode::Open);
        PoDoFo::PdfMemDocument pdf;
        const QByteArray utf8Password = password.toUtf8();
        pdf.Load(inputOutput, utf8Password.toStdString());
        // A PDF /Link is an annotation. A user password is therefore sufficient
        // when the PDF's permissions explicitly allow editing annotations.
        if (pdf.IsEncrypted() && !pdf.HasOwnerPermissions() && !pdf.IsEditNotesAllowed()) {
            QFile::remove(outputFileName);
            result.insert(QStringLiteral("message"), i18n("The encrypted PDF does not allow adding or editing annotations."));
            return result;
        }
        if (static_cast<unsigned>(sourceSelection.page) >= pdf.GetPages().GetCount()
            || static_cast<unsigned>(targetSelection.page) >= pdf.GetPages().GetCount()) {
            QFile::remove(outputFileName);
            result.insert(QStringLiteral("message"), i18n("The PDF text selection is outside the document."));
            return result;
        }

        PoDoFo::PdfPage &sourcePage = pdf.GetPages().GetPageAt(static_cast<unsigned>(sourceSelection.page));
        QVector<PoDoFo::Rect> sourceRectangles;
        sourceRectangles.reserve(sourceSelection.rectangles.size());
        for (const PdfNormalizedRectangle &rectangle : sourceSelection.rectangles) {
            sourceRectangles.append(pdfRectangleForNormalizedRectangle(sourcePage, rectangle));
        }
        if (sourceRectangles.isEmpty()) {
            QFile::remove(outputFileName);
            result.insert(QStringLiteral("message"), i18n("The PDF link source is empty."));
            return result;
        }

        PoDoFo::Rect sourceBounds = sourceRectangles.constFirst();
        for (qsizetype index = 1; index < sourceRectangles.size(); ++index) {
            sourceBounds = PoDoFo::Rect::FromCorners(std::min(sourceBounds.GetLeft(), sourceRectangles.at(index).GetLeft()),
                                                      std::min(sourceBounds.GetBottom(), sourceRectangles.at(index).GetBottom()),
                                                      std::max(sourceBounds.GetRight(), sourceRectangles.at(index).GetRight()),
                                                      std::max(sourceBounds.GetTop(), sourceRectangles.at(index).GetTop()));
        }

        PoDoFo::PdfPage &targetPage = pdf.GetPages().GetPageAt(static_cast<unsigned>(targetSelection.page));
        const PoDoFo::Rect targetRectangle = pdfRectangleForNormalizedRectangle(targetPage, targetSelection.rectangles.constFirst());
        auto destination = pdf.CreateDestination();
        destination->SetDestination(targetPage, targetRectangle.GetLeft(), targetRectangle.GetTop(), 0);

        PoDoFo::PdfAnnotationLink &link = sourcePage.GetAnnotations().CreateAnnot<PoDoFo::PdfAnnotationLink>(sourceBounds);
        const PoDoFo::PdfArray quadPoints = pdfQuadPointsForRectangles(sourceRectangles);
        link.SetQuadPoints(quadPoints);
        link.SetDestination(*destination);
        link.SetBorderStyle(0, 0, 0);
        pdf.SaveUpdate(*inputOutput, PoDoFo::PdfSaveOptions::NoMetadataUpdate);
    } catch (const std::exception &error) {
        QFile::remove(outputFileName);
        result.insert(QStringLiteral("message"), i18n("Unable to write the PDF link: %1", QString::fromLocal8Bit(error.what())));
        return result;
    }

    const QString backupFileName = inputFileName + QStringLiteral(".arianna-link-backup");
    QFile::remove(backupFileName);
    const bool backupCreated = QFile::rename(inputFileName, backupFileName);
    const bool originalReplaced = backupCreated && QFile::rename(outputFileName, inputFileName);
    if (!originalReplaced) {
        QFile::remove(outputFileName);
        QFile::rename(backupFileName, inputFileName);
        result.insert(QStringLiteral("message"), i18n("Unable to replace the original PDF."));
        return result;
    }

    QFile::remove(backupFileName);
    result.insert(QStringLiteral("success"), true);
    result.insert(QStringLiteral("message"), i18n("The PDF link was written to the PDF."));
    return result;
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
    // The viewer restores a validated session password for the same book.
    if (!m_password.isEmpty()) {
        m_password.clear();
        Q_EMIT passwordChanged();
    }
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

bool OkularPdfDocument::pdfLinkAnnotationAllowed() const
{
    return m_pdfLinkAnnotationAllowed;
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
    if (m_password == password && !m_needsPassword) {
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

QVariantList OkularPdfDocument::searchText(const QString &text) const
{
    QVariantList matches;
    const QString query = text.trimmed();
    if (!m_document || !m_document->isOpened() || query.isEmpty()) {
        return matches;
    }

    for (int pageNumber = 0; pageNumber < m_pageCount; ++pageNumber) {
        const Okular::Page *okularPage = m_document->page(pageNumber);
        if (!okularPage) {
            continue;
        }
        if (!okularPage->hasTextPage()) {
            m_document->requestTextPage(pageNumber);
        }
        const QString pageText = okularPage->text(nullptr, Okular::TextPage::CentralPixelTextAreaInclusionBehaviour);
        const int searchId = qHash(query) + pageNumber + 1;
        qsizetype position = pageText.indexOf(query, 0, Qt::CaseInsensitive);
        auto area = std::unique_ptr<Okular::RegularAreaRect>(okularPage->findText(searchId,
                                                                                              query,
                                                                                              Okular::FromTop,
                                                                                              Qt::CaseInsensitive,
                                                                                              nullptr));
        while (area) {
            QVariantList rectangles;
            for (const auto &rect : *area) {
                QVariantMap rectangle;
                rectangle.insert(QStringLiteral("x"), rect.left);
                rectangle.insert(QStringLiteral("y"), rect.top);
                rectangle.insert(QStringLiteral("width"), rect.width());
                rectangle.insert(QStringLiteral("height"), rect.height());
                rectangles.append(rectangle);
            }
            const qsizetype excerptStart = std::max<qsizetype>(0, position - 60);
            const qsizetype excerptLength = std::min<qsizetype>(pageText.size() - excerptStart, query.size() + 120);
            matches.append(QVariantMap{{QStringLiteral("page"), pageNumber},
                                       {QStringLiteral("excerpt"), pageText.mid(excerptStart, excerptLength).simplified()},
                                       {QStringLiteral("rectangles"), rectangles}});
            position = pageText.indexOf(query, position + query.size(), Qt::CaseInsensitive);
            area.reset(okularPage->findText(searchId, query, Okular::NextResult, Qt::CaseInsensitive, nullptr));
        }
    }
    return matches;
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

QString OkularPdfDocument::selectedTextForSpeech() const
{
    return textForTranslation(selectedText()).simplified();
}

QVariantMap OkularPdfDocument::speechSelectionRange() const
{
    if (!m_document->isOpened()) {
        return {};
    }
    QList<int> pages = m_pagesWithTextSelection.values();
    std::sort(pages.begin(), pages.end());
    QVariantMap result;
    for (int pageNumber : pages) {
        const auto *page = m_document->page(pageNumber);
        const auto *selection = page ? page->textSelection() : nullptr;
        if (!selection) {
            continue;
        }
        if (!page->hasTextPage()) {
            m_document->requestTextPage(pageNumber);
        }
        int offset = 0;
        for (const auto &entity : page->words(nullptr, Okular::TextPage::CentralPixelTextAreaInclusionBehaviour)) {
            const QString text = entity.text();
            const auto center = entity.area().center();
            if (!text.trimmed().isEmpty() && selection->contains(center.x, center.y)) {
                if (result.isEmpty()) {
                    result.insert(QStringLiteral("page"), pageNumber);
                    result.insert(QStringLiteral("offset"), offset);
                }
                result.insert(QStringLiteral("endPage"), pageNumber);
                result.insert(QStringLiteral("endOffset"), offset + text.size());
            }
            offset += text.size();
        }
    }
    return result;
}

QVariantMap OkularPdfDocument::speechPageSegments(int pageNumber, int offset, int maximumSegments) const
{
    const auto *page = m_document->isOpened() && pageNumber >= 0 && pageNumber < m_pageCount ? m_document->page(pageNumber) : nullptr;
    if (!page) {
        return {};
    }
    if (!page->hasTextPage()) {
        m_document->requestTextPage(pageNumber);
    }
    const QString source = page->text(nullptr, Okular::TextPage::CentralPixelTextAreaInclusionBehaviour);
    int cursor = std::clamp(offset, 0, int(source.size()));
    maximumSegments = std::clamp(maximumSegments, 1, 24);
    QVariantList segments;
    QString boundaryText = source;
    for (QChar &character : boundaryText) {
        if (character == QLatin1Char('\n') || character == QLatin1Char('\r')
            || character == QChar(0x2028) || character == QChar(0x2029)) {
            character = QLatin1Char(' ');
        }
    }
    // PDF line endings describe layout, not sentence boundaries. Keep the
    // same UTF-16 length so speech positions still address the original text.
    QTextBoundaryFinder sentences(QTextBoundaryFinder::Sentence, boundaryText);
    int characters = 0;
    while (cursor < source.size() && segments.size() < maximumSegments && characters < 8192) {
        sentences.setPosition(cursor);
        const int sentenceStart = sentences.isAtBoundary() ? cursor : std::max(0, int(sentences.toPreviousBoundary()));
        sentences.setPosition(cursor);
        int end = sentences.toNextBoundary();
        if (end < 0) {
            end = source.size();
        }
        // Bound exceptionally long sentences without losing their identity.
        // Source offsets always refer to Okular text, before speech cleanup.
        if (end - cursor > 2048) {
            end = cursor + 2048;
            while (end > cursor && !source.at(end).isSpace()) {
                --end;
            }
            if (end == cursor) {
                end = cursor + 2048;
                if (source.at(end).isLowSurrogate()) {
                    --end;
                }
            }
        }
        const QString text = textForTranslation(source.mid(cursor, end - cursor)).simplified();
        if (!text.isEmpty()) {
            const QString pageId = QStringLiteral("pdf-page-%1").arg(pageNumber);
            segments.append(QVariantMap{{QStringLiteral("text"), text},
                {QStringLiteral("id"), QStringLiteral("%1:%2:%3").arg(pageId).arg(cursor).arg(end)},
                {QStringLiteral("sentenceId"), QStringLiteral("%1:%2").arg(pageId).arg(sentenceStart)},
                {QStringLiteral("blockId"), pageId}, {QStringLiteral("blockTag"), QStringLiteral("p")},
                {QStringLiteral("href"), pageId}, {QStringLiteral("page"), pageNumber},
                {QStringLiteral("sourceStart"), cursor}, {QStringLiteral("sourceEnd"), end}});
            characters += text.size();
        }
        cursor = end;
    }
    return {{QStringLiteral("segments"), segments}, {QStringLiteral("nextOffset"), cursor},
        {QStringLiteral("endOfPage"), cursor >= source.size()}};
}

QVariantMap OkularPdfDocument::textSelectionGeometry() const
{
    QVariantMap selection;
    if (!m_document->isOpened() || m_pagesWithTextSelection.size() != 1) {
        return selection;
    }

    const int pageNumber = *m_pagesWithTextSelection.constBegin();
    const Okular::Page *okularPage = m_document->page(pageNumber);
    const Okular::RegularAreaRect *area = okularPage ? okularPage->textSelection() : nullptr;
    if (!area || area->isNull()) {
        return selection;
    }

    QVariantList rectangles;
    for (const Okular::NormalizedRect &rectangle : *area) {
        const double left = std::clamp(rectangle.left, 0.0, 1.0);
        const double top = std::clamp(rectangle.top, 0.0, 1.0);
        const double right = std::clamp(rectangle.right, 0.0, 1.0);
        const double bottom = std::clamp(rectangle.bottom, 0.0, 1.0);
        if (right <= left || bottom <= top) {
            continue;
        }

        QVariantMap geometry;
        geometry.insert(QStringLiteral("x"), left);
        geometry.insert(QStringLiteral("y"), top);
        geometry.insert(QStringLiteral("width"), right - left);
        geometry.insert(QStringLiteral("height"), bottom - top);
        rectangles.append(geometry);
    }
    if (rectangles.isEmpty()) {
        return {};
    }

    selection.insert(QStringLiteral("page"), pageNumber);
    selection.insert(QStringLiteral("rects"), rectangles);
    selection.insert(QStringLiteral("text"), selectedText());
    return selection;
}

QVariantMap OkularPdfDocument::createPdfLinkAnnotation(const QVariantMap &sourceSelection,
                                                        const QVariantMap &targetSelection,
                                                        const QString &targetFileName)
{
    QVariantMap result;
    PdfLinkSelection source;
    PdfLinkSelection target;
    QString errorMessage;
    if (!parsePdfLinkSelection(sourceSelection, source, errorMessage) || !parsePdfLinkSelection(targetSelection, target, errorMessage)) {
        result.insert(QStringLiteral("message"), errorMessage);
        return result;
    }

    const QString inputFileName =
        !targetFileName.trimmed().isEmpty() ? QFileInfo(targetFileName).absoluteFilePath() : (m_source.isLocalFile() ? m_source.toLocalFile() : QString());
    return writePdfLinkAnnotation(inputFileName, source, target, m_password);
}

QVariantMap OkularPdfDocument::createPdfAnchorAnnotation(const QVariantMap &selection,
                                                          const QString &anchorId,
                                                          const QString &targetFileName)
{
    QVariantMap result;
    // QML creates PDF anchors directly from a selection. Generate the UUID at
    // this boundary when it does not already have one, while retaining a
    // caller-supplied id for imported or restored anchors.
    const QString normalizedAnchorId = anchorId.trimmed().isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
        : anchorId.trimmed();

    PdfLinkSelection parsedSelection;
    QString errorMessage;
    if (!parsePdfLinkSelection(selection, parsedSelection, errorMessage)) {
        result.insert(QStringLiteral("message"), errorMessage);
        return result;
    }

    const QString inputFileName =
        !targetFileName.trimmed().isEmpty() ? QFileInfo(targetFileName).absoluteFilePath() : (m_source.isLocalFile() ? m_source.toLocalFile() : QString());
    return writePdfAriannaAnchorAnnotation(inputFileName, parsedSelection, normalizedAnchorId, m_password);
}

QVariantList OkularPdfDocument::pdfAriannaAnchors(const QString &targetFileName) const
{
    const QString inputFileName =
        !targetFileName.trimmed().isEmpty() ? QFileInfo(targetFileName).absoluteFilePath() : (m_source.isLocalFile() ? m_source.toLocalFile() : QString());
    if (inputFileName.isEmpty() || !QFileInfo(inputFileName).isFile()) {
        return {};
    }

    try {
        const QByteArray utf8Password = m_password.toUtf8();
        PoDoFo::PdfMemDocument pdf;
        pdf.Load(inputFileName.toStdString(), utf8Password.toStdString());

        QVariantList anchors;
        const unsigned pageCount = pdf.GetPages().GetCount();
        for (unsigned pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
            const PoDoFo::PdfPage &page = pdf.GetPages().GetPageAt(pageIndex);
            const auto &annotations = page.GetAnnotations();
            for (unsigned annotationIndex = 0; annotationIndex < annotations.GetCount(); ++annotationIndex) {
                const PoDoFo::PdfAnnotation &annotation = annotations.GetAnnotAt(annotationIndex);
                const QString anchorId = ariannaPdfAnchorId(annotation);
                if (anchorId.trimmed().isEmpty()) {
                    continue;
                }

                QVariantMap anchor;
                anchor.insert(QStringLiteral("anchorId"), anchorId);
                anchor.insert(QStringLiteral("page"), static_cast<int>(pageIndex));
                anchor.insert(QStringLiteral("rects"), normalizedRectanglesForAriannaAnchor(page, annotation));
                anchors.append(anchor);
            }
        }
        return anchors;
    } catch (const std::exception &error) {
        qWarning() << "Unable to read Arianna PDF anchors:" << inputFileName << error.what();
        return {};
    }
}

QVariantMap OkularPdfDocument::writeTableOfContentsToPdf(const QString &tableOfContentsJson, const QString &targetFileName)
{
    QVariantMap result;
    const QString inputFileName =
        !targetFileName.trimmed().isEmpty() ? QFileInfo(targetFileName).absoluteFilePath() : (m_source.isLocalFile() ? m_source.toLocalFile() : QString());
    const QJsonDocument tableOfContentsDocument = QJsonDocument::fromJson(tableOfContentsJson.toUtf8());
    if (!tableOfContentsDocument.isArray()) {
        result.insert(QStringLiteral("message"), i18n("The table of contents is invalid."));
        return result;
    }

    return mutatePdfOutline(
        inputFileName,
        [&tableOfContentsDocument](PoDoFo::PdfOutlines &outlines, PoDoFo::PdfMemDocument &pdf, QString &errorMessage) {
            return insertPdfOutlineItems(outlines, pdf, tableOfContentsDocument.array(), errorMessage);
        },
        i18n("The table of contents was written to the PDF."));
}

QVariantMap OkularPdfDocument::removeTableOfContentsItemFromPdf(const QString &tocId, const QString &targetFileName)
{
    const QString inputFileName =
        !targetFileName.trimmed().isEmpty() ? QFileInfo(targetFileName).absoluteFilePath() : (m_source.isLocalFile() ? m_source.toLocalFile() : QString());
    return mutatePdfOutline(
        inputFileName,
        [tocId](PoDoFo::PdfOutlines &outlines, PoDoFo::PdfMemDocument &, QString &errorMessage) {
            PdfOutlineStructure structure;
            PoDoFo::PdfOutlineItem *item = nullptr;
            if (!findPdfOutlineItem(outlines, tocId, structure, item, errorMessage)) {
                return false;
            }

            item->Erase();
            return true;
        },
        i18n("The table of contents item was removed from the PDF."));
}

QVariantMap OkularPdfDocument::editTableOfContentsItemInPdf(const QString &tocId, const QString &title, const QString &targetFileName)
{
    QVariantMap result;
    const QString trimmedTitle = title.trimmed();
    if (trimmedTitle.isEmpty()) {
        result.insert(QStringLiteral("message"), i18n("The table of contents title must not be empty."));
        return result;
    }

    const QString inputFileName =
        !targetFileName.trimmed().isEmpty() ? QFileInfo(targetFileName).absoluteFilePath() : (m_source.isLocalFile() ? m_source.toLocalFile() : QString());
    return mutatePdfOutline(
        inputFileName,
        [tocId, trimmedTitle](PoDoFo::PdfOutlines &outlines, PoDoFo::PdfMemDocument &, QString &errorMessage) {
            PdfOutlineStructure structure;
            PoDoFo::PdfOutlineItem *item = nullptr;
            if (!findPdfOutlineItem(outlines, tocId, structure, item, errorMessage)) {
                return false;
            }

            item->SetTitle(pdfStringFromQString(trimmedTitle));
            return true;
        },
        i18n("The table of contents item was updated in the PDF."));
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
    updatePdfLinkAnnotationPermission();
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
    setPdfLinkAnnotationAllowed(false);
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

void OkularPdfDocument::updatePdfLinkAnnotationPermission()
{
    // Use the document already opened by Okular. Loading the same PDF again
    // through PoDoFo can emit parser warnings for files Okular has repaired.
    setPdfLinkAnnotationAllowed(m_document && m_document->isOpened() && m_document->isAllowed(Okular::AllowNotes));
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

void OkularPdfDocument::setPdfLinkAnnotationAllowed(bool allowed)
{
    if (m_pdfLinkAnnotationAllowed == allowed) {
        return;
    }

    m_pdfLinkAnnotationAllowed = allowed;
    Q_EMIT pdfLinkAnnotationAllowedChanged();
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
