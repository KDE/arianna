// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

#include "okularpdfpageitem.h"

#include "okularpdfdocument.h"
#include "okularpdfpagepainter.h"

#include <QCursor>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPair>
#include <QQuickWindow>
#include <QRect>
#include <QSGSimpleRectNode>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <utility>

#include <core/action.h>
#include <core/document.h>
#include <core/generator.h>
#include <core/misc.h>
#include <core/page.h>

namespace
{
constexpr int PageViewPriority = 1;
constexpr int RedrawTimeout = 250;
constexpr int MaxRegionAnalysisPixels = 700000;

Okular::NormalizedPoint rotateInNormRect(const QPoint rotated, const QRect rect, Okular::Rotation rotation)
{
    switch (rotation) {
    case Okular::Rotation0:
        return Okular::NormalizedPoint(rotated.x(), rotated.y(), rect.width(), rect.height());
    case Okular::Rotation90:
        return Okular::NormalizedPoint(rotated.y(), rect.width() - rotated.x(), rect.height(), rect.width());
    case Okular::Rotation180:
        return Okular::NormalizedPoint(rect.width() - rotated.x(), rect.height() - rotated.y(), rect.width(), rect.height());
    case Okular::Rotation270:
        return Okular::NormalizedPoint(rect.height() - rotated.y(), rotated.x(), rect.height(), rect.width());
    }

    return Okular::NormalizedPoint();
}

QRectF normalizedRegionFromVariant(const QVariant &value)
{
    if (value.canConvert<QRectF>()) {
        const QRectF rect = value.toRectF();
        if (rect.isValid()) {
            const qreal left = std::clamp(std::min(rect.x(), rect.x() + rect.width()), 0.0, 1.0);
            const qreal top = std::clamp(std::min(rect.y(), rect.y() + rect.height()), 0.0, 1.0);
            const qreal right = std::clamp(std::max(rect.x(), rect.x() + rect.width()), 0.0, 1.0);
            const qreal bottom = std::clamp(std::max(rect.y(), rect.y() + rect.height()), 0.0, 1.0);
            if (right - left >= 0.002 && bottom - top >= 0.002) {
                return QRectF(QPointF(left, top), QPointF(right, bottom));
            }
        }
    }

    const QVariantMap map = value.toMap();
    bool okX = false;
    bool okY = false;
    bool okWidth = false;
    bool okHeight = false;
    const qreal x = map.value(QStringLiteral("x")).toReal(&okX);
    const qreal y = map.value(QStringLiteral("y")).toReal(&okY);
    const qreal width = map.value(QStringLiteral("width")).toReal(&okWidth);
    const qreal height = map.value(QStringLiteral("height")).toReal(&okHeight);
    if (!okX || !okY || !okWidth || !okHeight || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height)) {
        return {};
    }

    const qreal left = std::clamp(std::min(x, x + width), 0.0, 1.0);
    const qreal top = std::clamp(std::min(y, y + height), 0.0, 1.0);
    const qreal right = std::clamp(std::max(x, x + width), 0.0, 1.0);
    const qreal bottom = std::clamp(std::max(y, y + height), 0.0, 1.0);
    if (right - left < 0.002 || bottom - top < 0.002) {
        return {};
    }

    return QRectF(QPointF(left, top), QPointF(right, bottom));
}

QVariantMap variantMapFromNormalizedRegion(const QRectF &region)
{
    QVariantMap result;
    result.insert(QStringLiteral("x"), region.x());
    result.insert(QStringLiteral("y"), region.y());
    result.insert(QStringLiteral("width"), region.width());
    result.insert(QStringLiteral("height"), region.height());
    return result;
}

QRect pixelRectFromNormalizedRegion(const QRectF &region, const QSize &imageSize)
{
    if (region.isEmpty() || imageSize.isEmpty()) {
        return {};
    }

    const int imageWidth = imageSize.width();
    const int imageHeight = imageSize.height();
    const int left = std::clamp(static_cast<int>(std::floor(region.left() * imageWidth)), 0, imageWidth - 1);
    const int top = std::clamp(static_cast<int>(std::floor(region.top() * imageHeight)), 0, imageHeight - 1);
    const int right = std::clamp(static_cast<int>(std::ceil(region.right() * imageWidth)) - 1, left, imageWidth - 1);
    const int bottom = std::clamp(static_cast<int>(std::ceil(region.bottom() * imageHeight)) - 1, top, imageHeight - 1);
    return QRect(QPoint(left, top), QPoint(right, bottom));
}

int medianValue(QVector<int> values, int fallback = 0)
{
    if (values.isEmpty()) {
        return fallback;
    }

    const int middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    return values.at(middle);
}

int channelDistance(QRgb first, QRgb second)
{
    return std::max({std::abs(qRed(first) - qRed(second)), std::abs(qGreen(first) - qGreen(second)), std::abs(qBlue(first) - qBlue(second))});
}

int colorChroma(QRgb pixel)
{
    const int maximum = std::max({qRed(pixel), qGreen(pixel), qBlue(pixel)});
    const int minimum = std::min({qRed(pixel), qGreen(pixel), qBlue(pixel)});
    return maximum - minimum;
}

struct BackgroundEstimate {
    QRgb color = qRgb(255, 255, 255);
    int noise = 0;
    int chroma = 0;
};

BackgroundEstimate estimateBackgroundColor(const QImage &image)
{
    BackgroundEstimate result;
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return result;
    }

    QVector<int> redValues;
    QVector<int> greenValues;
    QVector<int> blueValues;
    QVector<QRgb> samples;
    const int step = std::max(1, std::min(image.width(), image.height()) / 160);

    auto addSample = [&](int x, int y) {
        const QRgb pixel = image.pixel(std::clamp(x, 0, image.width() - 1), std::clamp(y, 0, image.height() - 1));
        samples.append(pixel);
        redValues.append(qRed(pixel));
        greenValues.append(qGreen(pixel));
        blueValues.append(qBlue(pixel));
    };

    for (int x = 0; x < image.width(); x += step) {
        addSample(x, 0);
        addSample(x, image.height() - 1);
    }
    for (int y = step; y < image.height() - step; y += step) {
        addSample(0, y);
        addSample(image.width() - 1, y);
    }

    result.color = qRgb(medianValue(redValues, 255), medianValue(greenValues, 255), medianValue(blueValues, 255));
    result.chroma = colorChroma(result.color);

    QVector<int> deviations;
    deviations.reserve(samples.size());
    for (const QRgb pixel : std::as_const(samples)) {
        deviations.append(channelDistance(pixel, result.color));
    }
    result.noise = medianValue(deviations);
    return result;
}

bool isStrongColorPixel(QRgb pixel, const BackgroundEstimate &background)
{
    const int chroma = colorChroma(pixel);
    const int distance = channelDistance(pixel, background.color);
    return distance >= 18 && chroma >= std::max(34, background.chroma + 18);
}

bool isForegroundPixel(QRgb pixel, const BackgroundEstimate &background, int threshold)
{
    const int distance = channelDistance(pixel, background.color);
    const int totalDistance =
        std::abs(qRed(pixel) - qRed(background.color)) + std::abs(qGreen(pixel) - qGreen(background.color)) + std::abs(qBlue(pixel) - qBlue(background.color));
    return distance >= threshold || totalDistance >= threshold * 2 || isStrongColorPixel(pixel, background);
}

struct ContentBounds {
    QRect rect;
    int pixels = 0;
};

ContentBounds contentBoundsInRect(const QImage &image, QRect rect, const BackgroundEstimate &background, bool strongColorOnly, int threshold)
{
    ContentBounds result;
    rect = rect.intersected(image.rect());
    if (rect.isEmpty()) {
        return result;
    }

    int left = rect.right();
    int top = rect.bottom();
    int right = rect.left();
    int bottom = rect.top();

    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = rect.left(); x <= rect.right(); ++x) {
            const QRgb pixel = line[x];
            const bool matches = strongColorOnly ? isStrongColorPixel(pixel, background) : isForegroundPixel(pixel, background, threshold);
            if (!matches) {
                continue;
            }

            ++result.pixels;
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x);
            bottom = std::max(bottom, y);
        }
    }

    if (result.pixels > 0) {
        result.rect = QRect(QPoint(left, top), QPoint(right, bottom));
    }
    return result;
}

ContentBounds growContentBounds(const QImage &image,
                                ContentBounds bounds,
                                const BackgroundEstimate &background,
                                bool strongColorOnly,
                                int threshold,
                                QSize maximumGrowth,
                                QRect anchorRect)
{
    if (bounds.rect.isEmpty()) {
        return bounds;
    }

    anchorRect = anchorRect.intersected(image.rect());
    if (anchorRect.isEmpty()) {
        return bounds;
    }

    const int edgeTolerance = std::max(1, std::min(anchorRect.width(), anchorRect.height()) / 200);
    const int growthX = std::max(1, maximumGrowth.width());
    const int growthY = std::max(1, maximumGrowth.height());

    for (int pass = 0; pass < 4; ++pass) {
        const bool growLeft = bounds.rect.left() <= anchorRect.left() + edgeTolerance;
        const bool growTop = bounds.rect.top() <= anchorRect.top() + edgeTolerance;
        const bool growRight = bounds.rect.right() >= anchorRect.right() - edgeTolerance;
        const bool growBottom = bounds.rect.bottom() >= anchorRect.bottom() - edgeTolerance;
        if (!growLeft && !growTop && !growRight && !growBottom) {
            break;
        }

        const QRect candidate =
            bounds.rect.adjusted(growLeft ? -growthX : 0, growTop ? -growthY : 0, growRight ? growthX : 0, growBottom ? growthY : 0).intersected(image.rect());
        const ContentBounds grown = contentBoundsInRect(image, candidate, background, strongColorOnly, threshold);
        if (grown.rect.isEmpty()) {
            break;
        }

        QRect combined = bounds.rect.united(grown.rect);
        if (!growLeft) {
            combined.setLeft(bounds.rect.left());
        }
        if (!growTop) {
            combined.setTop(bounds.rect.top());
        }
        if (!growRight) {
            combined.setRight(bounds.rect.right());
        }
        if (!growBottom) {
            combined.setBottom(bounds.rect.bottom());
        }
        combined = combined.intersected(image.rect());
        if (combined.isEmpty() || combined == bounds.rect) {
            break;
        }

        bounds.rect = combined;
        bounds.pixels = grown.pixels;
    }
    return bounds;
}

QPair<qreal, qreal> constrainedSpan(qreal candidateStart, qreal candidateEnd, qreal originalStart, qreal originalEnd, qreal limit)
{
    const qreal originalSize = std::max<qreal>(1.0, originalEnd - originalStart);
    const qreal maximumShift = std::max<qreal>(1.0, originalSize * 0.05);
    const qreal minimumSize = std::max<qreal>(1.0, originalSize * 0.95);
    const qreal maximumSize = originalSize * 1.05;

    qreal start = std::clamp(candidateStart, originalStart - maximumShift, originalStart + maximumShift);
    qreal end = std::clamp(candidateEnd, originalEnd - maximumShift, originalEnd + maximumShift);
    if (end < start) {
        std::swap(start, end);
    }

    qreal size = end - start;
    if (size < minimumSize || size > maximumSize) {
        const qreal targetSize = std::clamp(size, minimumSize, maximumSize);
        qreal center = (start + end) / 2.0;
        center = std::clamp(center, originalStart - maximumShift + targetSize / 2.0, originalEnd + maximumShift - targetSize / 2.0);
        start = center - targetSize / 2.0;
        end = center + targetSize / 2.0;
    }

    if (start < 0) {
        end -= start;
        start = 0;
    }
    if (end > limit) {
        start -= end - limit;
        end = limit;
    }

    start = std::clamp(start, 0.0, limit);
    end = std::clamp(end, start, limit);
    return qMakePair(start, end);
}

QRectF constrainedRegionForSelection(qreal candidateLeft,
                                     qreal candidateTop,
                                     qreal candidateRight,
                                     qreal candidateBottom,
                                     const QRect &selectedRect,
                                     const QSize &imageSize)
{
    if (selectedRect.isEmpty() || imageSize.isEmpty()) {
        return {};
    }

    const qreal originalLeft = selectedRect.left();
    const qreal originalTop = selectedRect.top();
    const qreal originalRight = selectedRect.right() + 1.0;
    const qreal originalBottom = selectedRect.bottom() + 1.0;

    const auto horizontal = constrainedSpan(candidateLeft, candidateRight, originalLeft, originalRight, imageSize.width());
    const auto vertical = constrainedSpan(candidateTop, candidateBottom, originalTop, originalBottom, imageSize.height());
    return QRectF(QPointF(horizontal.first, vertical.first), QPointF(horizontal.second, vertical.second));
}
}

OkularPdfPageItem::OkularPdfPageItem(QQuickItem *parent)
    : QQuickItem(parent)
    , Okular::View(QStringLiteral("AriannaOkularPdfPageItem"))
{
    setFlag(QQuickItem::ItemHasContents, true);
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);

    m_redrawTimer = new QTimer(this);
    m_redrawTimer->setInterval(RedrawTimeout);
    m_redrawTimer->setSingleShot(true);
    connect(m_redrawTimer, &QTimer::timeout, this, &OkularPdfPageItem::requestPixmap);
    connect(this, &QQuickItem::windowChanged, this, [this]() {
        schedulePixmapRequest();
    });
}

OkularPdfPageItem::~OkularPdfPageItem() = default;

OkularPdfDocument *OkularPdfPageItem::document() const
{
    return m_document.data();
}

void OkularPdfPageItem::setDocument(OkularPdfDocument *document)
{
    if (m_document.data() == document) {
        return;
    }

    if (m_document) {
        disconnect(m_document.data(), nullptr, this, nullptr);
    }

    m_document = document;
    if (m_document) {
        connect(m_document.data(), &OkularPdfDocument::openedChanged, this, &OkularPdfPageItem::refreshPage);
        connect(m_document.data(), &OkularPdfDocument::pageCountChanged, this, &OkularPdfPageItem::refreshPage);
        connect(m_document.data(), &OkularPdfDocument::sourceChanged, this, &OkularPdfPageItem::refreshPage);
        connect(m_document.data(), &OkularPdfDocument::pageChanged, this, &OkularPdfPageItem::handlePageChanged);
        connect(m_document.data(), &QObject::destroyed, this, [this]() {
            m_document.clear();
            refreshPage();
        });
    }

    refreshPage();
    Q_EMIT documentChanged();
}

int OkularPdfPageItem::pageNumber() const
{
    return m_pageNumber;
}

void OkularPdfPageItem::setPageNumber(int pageNumber)
{
    if (m_pageNumber == pageNumber) {
        return;
    }

    m_pageNumber = pageNumber;
    m_mousePressed = false;
    m_textSelecting = false;
    m_pressedLinkAction = nullptr;
    m_hasLastSelectionEndPosition = false;
    m_textSelectionRects.clear();
    refreshPage();
    unsetCursor();
    Q_EMIT pageNumberChanged();
}

bool OkularPdfPageItem::pageValid() const
{
    return m_pageValid;
}

bool OkularPdfPageItem::inverted() const
{
    return m_inverted;
}

void OkularPdfPageItem::setInverted(bool inverted)
{
    if (m_inverted == inverted) {
        return;
    }

    m_inverted = inverted;
    if (m_pageValid) {
        paint();
    }
    Q_EMIT invertedChanged();
}

bool OkularPdfPageItem::recolor() const
{
    return m_recolor;
}

void OkularPdfPageItem::setRecolor(bool recolor)
{
    if (m_recolor == recolor) {
        return;
    }

    m_recolor = recolor;
    if (m_pageValid) {
        paint();
    }
    Q_EMIT recolorChanged();
}

bool OkularPdfPageItem::transparentPageBackground() const
{
    return m_transparentPageBackground;
}

void OkularPdfPageItem::setTransparentPageBackground(bool transparentPageBackground)
{
    if (m_transparentPageBackground == transparentPageBackground) {
        return;
    }

    m_transparentPageBackground = transparentPageBackground;
    if (m_pageValid) {
        paint();
    }
    Q_EMIT transparentPageBackgroundChanged();
}

QColor OkularPdfPageItem::foregroundColor() const
{
    return m_foregroundColor;
}

void OkularPdfPageItem::setForegroundColor(const QColor &foregroundColor)
{
    if (m_foregroundColor == foregroundColor) {
        return;
    }

    m_foregroundColor = foregroundColor;
    if (m_recolor && m_pageValid) {
        paint();
    }
    Q_EMIT foregroundColorChanged();
}

QColor OkularPdfPageItem::backgroundColor() const
{
    return m_backgroundColor;
}

void OkularPdfPageItem::setBackgroundColor(const QColor &backgroundColor)
{
    if (m_backgroundColor == backgroundColor) {
        return;
    }

    m_backgroundColor = backgroundColor;
    if (m_recolor && m_pageValid) {
        paint();
    }
    Q_EMIT backgroundColorChanged();
}

QVariantList OkularPdfPageItem::notInvertedRegions() const
{
    return m_notInvertedRegions;
}

void OkularPdfPageItem::setNotInvertedRegions(const QVariantList &regions)
{
    if (m_notInvertedRegions == regions) {
        return;
    }

    m_notInvertedRegions = regions;
    m_notInvertedRegionRects.clear();
    for (const QVariant &region : regions) {
        const QRectF normalizedRegion = normalizedRegionFromVariant(region);
        if (!normalizedRegion.isEmpty()) {
            const bool elliptical = region.toMap().value(QStringLiteral("shape")).toString() == QLatin1String("ellipse");
            m_notInvertedRegionRects.append({normalizedRegion, elliptical});
        }
    }

    if ((m_inverted || m_recolor) && m_pageValid) {
        paint();
    }
    Q_EMIT notInvertedRegionsChanged();
}

QVariantList OkularPdfPageItem::forcedInvertedRegions() const
{
    return m_forcedInvertedRegions;
}

void OkularPdfPageItem::setForcedInvertedRegions(const QVariantList &regions)
{
    if (m_forcedInvertedRegions == regions) {
        return;
    }

    m_forcedInvertedRegions = regions;
    m_forcedInvertedRegionRects.clear();
    for (const QVariant &region : regions) {
        const QRectF normalizedRegion = normalizedRegionFromVariant(region);
        if (!normalizedRegion.isEmpty()) {
            const bool elliptical = region.toMap().value(QStringLiteral("shape")).toString() == QLatin1String("ellipse");
            m_forcedInvertedRegionRects.append({normalizedRegion, elliptical});
        }
    }

    if ((m_inverted || m_recolor) && m_pageValid) {
        paint();
    }
    Q_EMIT forcedInvertedRegionsChanged();
}

QVariantMap OkularPdfPageItem::refinedColorRegion(const QVariant &region) const
{
    const QRectF normalizedRegion = normalizedRegionFromVariant(region);
    const QVariantMap fallback = variantMapFromNormalizedRegion(normalizedRegion);
    if (normalizedRegion.isEmpty() || m_buffer.isNull()) {
        return fallback;
    }

    const QImage pageImage = m_buffer.convertToFormat(QImage::Format_RGB32);
    const QRect selectedPixelRect = pixelRectFromNormalizedRegion(normalizedRegion, pageImage.size());
    if (selectedPixelRect.width() < 4 || selectedPixelRect.height() < 4) {
        return fallback;
    }

    const int horizontalGrowth = std::max(1, static_cast<int>(std::ceil(selectedPixelRect.width() * 0.05)));
    const int verticalGrowth = std::max(1, static_cast<int>(std::ceil(selectedPixelRect.height() * 0.05)));
    const QRect searchRect = selectedPixelRect.adjusted(-horizontalGrowth, -verticalGrowth, horizontalGrowth, verticalGrowth).intersected(pageImage.rect());
    if (searchRect.width() < 4 || searchRect.height() < 4) {
        return fallback;
    }

    QImage analysisImage = pageImage.copy(searchRect);
    qreal sourcePixelsPerAnalysisPixelX = 1.0;
    qreal sourcePixelsPerAnalysisPixelY = 1.0;
    const qint64 searchPixelCount = static_cast<qint64>(analysisImage.width()) * analysisImage.height();
    if (searchPixelCount > MaxRegionAnalysisPixels) {
        const qreal scale = std::sqrt(static_cast<qreal>(MaxRegionAnalysisPixels) / static_cast<qreal>(searchPixelCount));
        const QSize scaledSize(std::max(1, static_cast<int>(std::floor(analysisImage.width() * scale))),
                               std::max(1, static_cast<int>(std::floor(analysisImage.height() * scale))));
        sourcePixelsPerAnalysisPixelX = static_cast<qreal>(analysisImage.width()) / scaledSize.width();
        sourcePixelsPerAnalysisPixelY = static_cast<qreal>(analysisImage.height()) / scaledSize.height();
        analysisImage = analysisImage.scaled(scaledSize, Qt::IgnoreAspectRatio, Qt::FastTransformation).convertToFormat(QImage::Format_RGB32);
    }

    const QRect originalAnalysisRect(
        std::clamp(static_cast<int>(std::floor((selectedPixelRect.left() - searchRect.left()) / sourcePixelsPerAnalysisPixelX)), 0, analysisImage.width() - 1),
        std::clamp(static_cast<int>(std::floor((selectedPixelRect.top() - searchRect.top()) / sourcePixelsPerAnalysisPixelY)), 0, analysisImage.height() - 1),
        std::max(1, static_cast<int>(std::ceil(selectedPixelRect.width() / sourcePixelsPerAnalysisPixelX))),
        std::max(1, static_cast<int>(std::ceil(selectedPixelRect.height() / sourcePixelsPerAnalysisPixelY))));
    const QRect boundedOriginalAnalysisRect = originalAnalysisRect.intersected(analysisImage.rect());
    if (boundedOriginalAnalysisRect.width() < 4 || boundedOriginalAnalysisRect.height() < 4) {
        return fallback;
    }

    const BackgroundEstimate background = estimateBackgroundColor(analysisImage);
    const int foregroundThreshold = std::clamp(background.noise * 3 + 24, 26, 72);
    const int minimumColorPixels = std::max(12, boundedOriginalAnalysisRect.width() * boundedOriginalAnalysisRect.height() / 900);

    ContentBounds bounds = contentBoundsInRect(analysisImage, boundedOriginalAnalysisRect, background, true, foregroundThreshold);
    bool strongColorOnly = true;
    if (bounds.pixels < minimumColorPixels) {
        strongColorOnly = false;
        bounds = contentBoundsInRect(analysisImage, boundedOriginalAnalysisRect, background, false, foregroundThreshold);
        if (bounds.pixels < minimumColorPixels) {
            return fallback;
        }
    }

    const QSize maximumGrowth(std::max(1, static_cast<int>(std::ceil(boundedOriginalAnalysisRect.width() * 0.03))),
                              std::max(1, static_cast<int>(std::ceil(boundedOriginalAnalysisRect.height() * 0.03))));
    bounds = growContentBounds(analysisImage, bounds, background, strongColorOnly, foregroundThreshold, maximumGrowth, boundedOriginalAnalysisRect);

    if (bounds.rect.width() < 3 || bounds.rect.height() < 3) {
        return fallback;
    }

    const QRect refinedAnalysisRect = bounds.rect.intersected(analysisImage.rect());
    if (refinedAnalysisRect.width() < 4 || refinedAnalysisRect.height() < 4) {
        return fallback;
    }

    const qreal refinedLeft = searchRect.left() + refinedAnalysisRect.left() * sourcePixelsPerAnalysisPixelX;
    const qreal refinedTop = searchRect.top() + refinedAnalysisRect.top() * sourcePixelsPerAnalysisPixelY;
    const qreal refinedRight = searchRect.left() + (refinedAnalysisRect.right() + 1) * sourcePixelsPerAnalysisPixelX;
    const qreal refinedBottom = searchRect.top() + (refinedAnalysisRect.bottom() + 1) * sourcePixelsPerAnalysisPixelY;

    const QRectF constrainedPixelRegion =
        constrainedRegionForSelection(refinedLeft, refinedTop, refinedRight, refinedBottom, selectedPixelRect, pageImage.size());
    const QRectF refinedNormalizedRegion(QPointF(std::clamp(constrainedPixelRegion.left() / pageImage.width(), 0.0, 1.0),
                                                 std::clamp(constrainedPixelRegion.top() / pageImage.height(), 0.0, 1.0)),
                                         QPointF(std::clamp(constrainedPixelRegion.right() / pageImage.width(), 0.0, 1.0),
                                                 std::clamp(constrainedPixelRegion.bottom() / pageImage.height(), 0.0, 1.0)));
    if (refinedNormalizedRegion.width() < 0.002 || refinedNormalizedRegion.height() < 0.002) {
        return fallback;
    }

    return variantMapFromNormalizedRegion(refinedNormalizedRegion);
}

QSGNode *OkularPdfPageItem::updatePaintNode(QSGNode *node, QQuickItem::UpdatePaintNodeData * /*data*/)
{
    if (!window() || m_buffer.isNull()) {
        delete node;
        return nullptr;
    }

    bool nodeCreated = false;
    if (!node || node->childCount() != 2) {
        delete node;
        node = new QSGNode();

        auto *textureNode = new QSGSimpleTextureNode();
        textureNode->setOwnsTexture(true);
        node->appendChildNode(textureNode);
        node->appendChildNode(new QSGNode());
        nodeCreated = true;
    }

    auto *textureNode = static_cast<QSGSimpleTextureNode *>(node->childAtIndex(0));
    QSGNode *selectionNode = node->childAtIndex(1);

    if (nodeCreated || m_textureDirty || !textureNode->texture()) {
        textureNode->setTexture(window()->createTextureFromImage(m_buffer));
        m_textureDirty = false;
    }
    textureNode->setRect(boundingRect());

    while (QSGNode *child = selectionNode->firstChild()) {
        selectionNode->removeChildNode(child);
        delete child;
    }

    for (const QRectF &selectionRect : std::as_const(m_textSelectionRects)) {
        selectionNode->appendChildNode(new QSGSimpleRectNode(selectionRect, m_textSelectionColor));
    }

    return node;
}

void OkularPdfPageItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        updateTextSelectionOverlay();
        schedulePixmapRequest();
    }
}

void OkularPdfPageItem::hoverMoveEvent(QHoverEvent *event)
{
    if (!event) {
        return;
    }

    updateCursorForPosition(event->position());
    QQuickItem::hoverMoveEvent(event);
}

void OkularPdfPageItem::hoverEnterEvent(QHoverEvent *event)
{
    if (!event) {
        return;
    }

    updateCursorForPosition(event->position());
    QQuickItem::hoverEnterEvent(event);
}

void OkularPdfPageItem::updateCursorForPosition(const QPointF &position)
{
    if (linkActionAt(position)) {
        setCursor(Qt::PointingHandCursor);
    } else {
        unsetCursor();
    }
}

void OkularPdfPageItem::hoverLeaveEvent(QHoverEvent *event)
{
    unsetCursor();
    QQuickItem::hoverLeaveEvent(event);
}

void OkularPdfPageItem::mousePressEvent(QMouseEvent *event)
{
    if (!event) {
        return;
    }

    if (!canSelectText()) {
        QQuickItem::mousePressEvent(event);
        return;
    }

    if (event->button() == Qt::LeftButton) {
        m_selectionStartPosition = event->position();
        m_mousePressed = true;
        m_textSelecting = false;
        m_pressedLinkAction = linkActionAt(event->position());
        m_hasLastSelectionEndPosition = false;
        m_document->clearTextSelection();
        event->accept();
        return;
    }

    if (event->button() == Qt::RightButton) {
        if (m_document->hasTextSelection()) {
            m_document->clearTextSelection();
        }
        Q_EMIT pageColorMenuRequested(event->position().x(), event->position().y());
        event->accept();
        return;
    }

    QQuickItem::mousePressEvent(event);
}

void OkularPdfPageItem::mouseMoveEvent(QMouseEvent *event)
{
    if (!event) {
        return;
    }

    if (!m_mousePressed || !(event->buttons() & Qt::LeftButton) || !canSelectText()) {
        QQuickItem::mouseMoveEvent(event);
        return;
    }

    if (!m_textSelecting && (event->position() - m_selectionStartPosition).manhattanLength() < 5) {
        event->accept();
        return;
    }

    m_textSelecting = true;
    updateTextSelection(event->position());
    event->accept();
}

void OkularPdfPageItem::mouseReleaseEvent(QMouseEvent *event)
{
    if (!event) {
        return;
    }

    if (event->button() != Qt::LeftButton || !m_mousePressed) {
        QQuickItem::mouseReleaseEvent(event);
        return;
    }

    bool requestSelectionMenu = false;
    bool linkActivated = false;
    if (m_textSelecting && canSelectText()) {
        updateTextSelection(event->position());
        requestSelectionMenu = m_document && m_document->hasTextSelection() && !m_document->selectedText().trimmed().isEmpty();
    } else if (m_pressedLinkAction && linkActionAt(event->position()) == m_pressedLinkAction) {
        linkActivated = activateLinkAction(m_pressedLinkAction, event->position());
    }

    const QPointF menuPosition = event->position();
    m_mousePressed = false;
    m_textSelecting = false;
    m_pressedLinkAction = nullptr;
    event->accept();

    if (!linkActivated && requestSelectionMenu) {
        Q_EMIT selectionMenuRequested(menuPosition.x(), menuPosition.y());
    }
}

void OkularPdfPageItem::refreshPage()
{
    const bool wasValid = m_pageValid;
    m_page = nullptr;
    m_pageValid = false;

    if (m_document && m_document->opened() && m_pageNumber >= 0 && m_pageNumber < m_document->pageCount()) {
        m_page = m_document->document()->page(m_pageNumber);
        m_pageValid = m_page != nullptr;
    }

    setImplicitWidth(m_page ? m_page->width() : 0);
    setImplicitHeight(m_page ? m_page->height() : 0);

    if (wasValid != m_pageValid) {
        Q_EMIT pageValidChanged();
    }

    updateTextSelectionOverlay();
    schedulePixmapRequest();
}

void OkularPdfPageItem::schedulePixmapRequest()
{
    if (!m_redrawTimer) {
        return;
    }

    if (!m_pageValid || width() <= 0 || height() <= 0) {
        clearBuffer();
        return;
    }

    m_redrawTimer->start();
}

void OkularPdfPageItem::requestPixmap()
{
    if (!m_document || !m_page || !window() || width() <= 0 || height() <= 0) {
        clearBuffer();
        return;
    }

    paint();

    const qreal dpr = window()->devicePixelRatio();
    const int targetWidth = std::max(1, static_cast<int>(std::ceil(width())));
    const int targetHeight = std::max(1, static_cast<int>(std::ceil(height())));

    auto *request = new Okular::PixmapRequest(m_document->pageviewObserver(),
                                              m_pageNumber,
                                              targetWidth,
                                              targetHeight,
                                              dpr,
                                              PageViewPriority,
                                              Okular::PixmapRequest::Asynchronous);
    request->setNormalizedRect(Okular::NormalizedRect(0, 0, 1, 1));
    m_document->document()->requestPixmaps({request}, Okular::Document::NoOption);
}

void OkularPdfPageItem::paint()
{
    if (!m_document || !m_page || !window() || width() <= 0 || height() <= 0) {
        clearBuffer();
        return;
    }

    const qreal dpr = window()->devicePixelRatio();
    const QSize logicalSize(std::max(1, static_cast<int>(std::ceil(width()))), std::max(1, static_cast<int>(std::ceil(height()))));
    const QSize deviceSize(std::max(1, static_cast<int>(std::ceil(logicalSize.width() * dpr))),
                           std::max(1, static_cast<int>(std::ceil(logicalSize.height() * dpr))));

    QPixmap pixmap(deviceSize);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(m_transparentPageBackground ? Qt::transparent : Qt::white);

    QPainter painter(&pixmap);
    PagePainter::paintPageOnPainter(&painter,
                                    m_page,
                                    m_document->pageviewObserver(),
                                    (m_recolor ? PagePainter::RecolorForegroundBackground : (m_inverted ? PagePainter::InvertColors : 0))
                                        | (m_transparentPageBackground ? PagePainter::TransparentPageBackground : 0),
                                    logicalSize.width(),
                                    logicalSize.height(),
                                    QRect(QPoint(0, 0), logicalSize),
                                    m_foregroundColor,
                                    m_backgroundColor,
                                    m_notInvertedRegionRects,
                                    m_forcedInvertedRegionRects);
    painter.end();

    m_buffer = pixmap.toImage();
    m_textureDirty = true;
    update();
}

void OkularPdfPageItem::clearBuffer()
{
    if (m_buffer.isNull()) {
        return;
    }

    m_buffer = QImage();
    m_textureDirty = true;
    m_textSelectionRects.clear();
    update();
}

bool OkularPdfPageItem::canSelectText() const
{
    return m_document && m_document->opened() && m_page && m_pageValid && width() > 0 && height() > 0;
}

bool OkularPdfPageItem::hasLinkAt(qreal x, qreal y) const
{
    return linkActionAt(QPointF(x, y)) != nullptr;
}

const Okular::Action *OkularPdfPageItem::linkActionAt(const QPointF &position) const
{
    if (!canSelectText()) {
        return nullptr;
    }

    const Okular::NormalizedPoint point = normalizedPointForPosition(position);
    const Okular::ObjectRect *rect = m_page->objectRect(Okular::ObjectRect::Action, point.x, point.y, width(), height());
    return rect ? static_cast<const Okular::Action *>(rect->object()) : nullptr;
}

const Okular::Action *OkularPdfPageItem::internalLinkActionAt(const QPointF &position) const
{
    const Okular::Action *action = linkActionAt(position);
    return action && m_document && !m_document->internalLocationForAction(action).isEmpty() ? action : nullptr;
}

bool OkularPdfPageItem::activateLinkAction(const Okular::Action *action, const QPointF &sourcePosition)
{
    if (!m_document || !action) {
        return false;
    }

    const QString location = m_document->internalLocationForAction(action);
    const Okular::NormalizedPoint sourcePoint = normalizedPointForPosition(sourcePosition);
    const QString sourceLocation = m_document->locationForPagePosition(m_pageNumber, sourcePoint.x, sourcePoint.y, Okular::DocumentViewport::Center);

    if (location.isEmpty()) {
        if (action->actionType() != Okular::Action::Browse) {
            return false;
        }

        const auto *browseAction = static_cast<const Okular::BrowseAction *>(action);
        const QUrl url = browseAction->url();
        if (!url.isValid() || url.isEmpty()) {
            return false;
        }

        Q_EMIT externalLinkActivated(url.toString(), sourceLocation);
        return true;
    }

    Q_EMIT linkActivated(location, sourceLocation);
    return true;
}

void OkularPdfPageItem::updateTextSelectionOverlay()
{
    m_textSelectionRects.clear();
    m_textSelectionColor = QColor(0, 96, 192, 96);

    if (!m_page || !m_pageValid || width() <= 0 || height() <= 0) {
        return;
    }

    const Okular::RegularAreaRect *textSelection = m_page->textSelection();
    if (!textSelection) {
        return;
    }

    const int pageWidth = std::max(1, static_cast<int>(std::ceil(width())));
    const int pageHeight = std::max(1, static_cast<int>(std::ceil(height())));
    const QRect pageRect(0, 0, pageWidth, pageHeight);

    QColor selectionColor = m_page->textSelectionColor();
    if (selectionColor.isValid()) {
        if (selectionColor.alpha() == 255) {
            selectionColor.setAlpha(96);
        }
        m_textSelectionColor = selectionColor;
    }

    const QList<QRect> selectionRects = textSelection->geometry(pageWidth, pageHeight);
    m_textSelectionRects.reserve(selectionRects.size());
    for (const QRect &selectionRect : selectionRects) {
        const QRect visibleRect = selectionRect.intersected(pageRect);
        if (!visibleRect.isEmpty()) {
            m_textSelectionRects.append(QRectF(visibleRect));
        }
    }
}

void OkularPdfPageItem::updateTextSelection(const QPointF &endPosition)
{
    if (!m_document) {
        return;
    }

    const QPoint roundedEndPosition(static_cast<int>(std::lround(endPosition.x())), static_cast<int>(std::lround(endPosition.y())));
    if (m_hasLastSelectionEndPosition && m_lastSelectionEndPosition == roundedEndPosition) {
        return;
    }

    m_lastSelectionEndPosition = roundedEndPosition;
    m_hasLastSelectionEndPosition = true;
    m_document->setTextSelection(m_pageNumber, textSelectionForPoints(m_selectionStartPosition, endPosition));
}

std::unique_ptr<Okular::RegularAreaRect> OkularPdfPageItem::textSelectionForPoints(const QPointF &startPosition, const QPointF &endPosition) const
{
    if (!canSelectText()) {
        return nullptr;
    }

    if (!m_page->hasTextPage()) {
        m_document->document()->requestTextPage(m_pageNumber);
    }

    const Okular::TextSelection selection(normalizedPointForPosition(startPosition), normalizedPointForPosition(endPosition));
    return m_page->textArea(selection);
}

Okular::NormalizedPoint OkularPdfPageItem::normalizedPointForPosition(const QPointF &position) const
{
    const int pageWidth = std::max(1, static_cast<int>(std::ceil(width())));
    const int pageHeight = std::max(1, static_cast<int>(std::ceil(height())));
    const int x = std::max(0, std::min(pageWidth, static_cast<int>(std::lround(position.x()))));
    const int y = std::max(0, std::min(pageHeight, static_cast<int>(std::lround(position.y()))));

    return rotateInNormRect(QPoint(x, y), QRect(0, 0, pageWidth, pageHeight), m_page ? m_page->rotation() : Okular::Rotation0);
}

void OkularPdfPageItem::handlePageChanged(int page, int flags)
{
    if (page != m_pageNumber) {
        return;
    }

    const bool pixmapChanged = flags & Okular::DocumentObserver::Pixmap;
    const bool textSelectionChanged = flags & Okular::DocumentObserver::TextSelection;

    if (pixmapChanged) {
        paint();
    }

    if (textSelectionChanged) {
        updateTextSelectionOverlay();
        update();
    }

    if (pixmapChanged || textSelectionChanged) {
        return;
    }

    schedulePixmapRequest();
}
