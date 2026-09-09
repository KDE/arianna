// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

#include "okularpdfpagepainter.h"

#include <QColor>
#include <QImage>
#include <QList>
#include <QPaintDevice>
#include <QPainter>
#include <QPixmap>
#include <QRect>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <utility>

#include <core/observer.h>
#include <core/page.h>

namespace
{
struct ImageColorRegion {
    QRect rect;
    bool elliptical = false;
};

QVector<ImageColorRegion> imageRegions(const QVector<PagePainter::ColorRegion> &regions, const QSize &imageSize)
{
    QVector<ImageColorRegion> imageRegions;
    if (regions.isEmpty() || imageSize.isEmpty()) {
        return imageRegions;
    }

    imageRegions.reserve(regions.size());
    const QRect imageRect(QPoint(0, 0), imageSize);
    for (const PagePainter::ColorRegion &region : regions) {
        if (region.rect.isEmpty()) {
            continue;
        }

        const QRect rect = QRectF(region.rect.x() * imageSize.width(),
                                  region.rect.y() * imageSize.height(),
                                  region.rect.width() * imageSize.width(),
                                  region.rect.height() * imageSize.height())
                               .toAlignedRect()
                               .intersected(imageRect);
        if (!rect.isEmpty()) {
            imageRegions.append({rect, region.elliptical});
        }
    }

    return imageRegions;
}

bool pointInRegions(const QVector<ImageColorRegion> &regions, int x, int y)
{
    for (const ImageColorRegion &region : regions) {
        if (!region.rect.contains(x, y)) {
            continue;
        }

        if (!region.elliptical) {
            return true;
        }

        const qreal horizontalDistance = 2.0 * (x + 0.5 - region.rect.x()) / region.rect.width() - 1.0;
        const qreal verticalDistance = 2.0 * (y + 0.5 - region.rect.y()) / region.rect.height() - 1.0;
        if (horizontalDistance * horizontalDistance + verticalDistance * verticalDistance <= 1.0) {
            return true;
        }
    }

    return false;
}

QImage recoloredImage(QImage image,
                      const QVector<ImageColorRegion> &preserveRegions,
                      const QVector<ImageColorRegion> &forceTransformRegions,
                      const QColor &foregroundColor,
                      const QColor &backgroundColor)
{
    if (image.format() != QImage::Format_ARGB32) {
        image = image.convertToFormat(QImage::Format_ARGB32);
    }

    const int foregroundRed = foregroundColor.red();
    const int foregroundGreen = foregroundColor.green();
    const int foregroundBlue = foregroundColor.blue();
    const int backgroundRed = backgroundColor.red();
    const int backgroundGreen = backgroundColor.green();
    const int backgroundBlue = backgroundColor.blue();
    const bool hasPreserveRegions = !preserveRegions.isEmpty();
    const bool hasForceTransformRegions = hasPreserveRegions && !forceTransformRegions.isEmpty();

    for (int y = 0; y < image.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            if (hasPreserveRegions) {
                const bool forceTransform = hasForceTransformRegions && pointInRegions(forceTransformRegions, x, y);
                if (!forceTransform && pointInRegions(preserveRegions, x, y)) {
                    continue;
                }
            }

            const int backgroundWeight = qGray(pixel);
            const int foregroundWeight = 255 - backgroundWeight;
            line[x] = qRgba((foregroundRed * foregroundWeight + backgroundRed * backgroundWeight) / 255,
                            (foregroundGreen * foregroundWeight + backgroundGreen * backgroundWeight) / 255,
                            (foregroundBlue * foregroundWeight + backgroundBlue * backgroundWeight) / 255,
                            qAlpha(pixel));
        }
    }

    return image;
}

QImage invertedImage(QImage image, const QVector<ImageColorRegion> &preserveRegions, const QVector<ImageColorRegion> &forceTransformRegions)
{
    if (image.format() != QImage::Format_ARGB32) {
        image = image.convertToFormat(QImage::Format_ARGB32);
    }

    if (preserveRegions.isEmpty()) {
        image.invertPixels(QImage::InvertRgb);
        return image;
    }

    const bool hasForceTransformRegions = !forceTransformRegions.isEmpty();
    for (int y = 0; y < image.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            const bool forceTransform = hasForceTransformRegions && pointInRegions(forceTransformRegions, x, y);
            if (!forceTransform && pointInRegions(preserveRegions, x, y)) {
                continue;
            }

            line[x] = qRgba(255 - qRed(pixel), 255 - qGreen(pixel), 255 - qBlue(pixel), qAlpha(pixel));
        }
    }

    return image;
}

int colorDistance(QRgb pixel, const QColor &color)
{
    return std::max({std::abs(qRed(pixel) - color.red()), std::abs(qGreen(pixel) - color.green()), std::abs(qBlue(pixel) - color.blue())});
}

int unmattedChannel(int channel, int paperChannel, int coverage)
{
    if (coverage <= 0) {
        return 0;
    }

    return std::clamp((channel * 255 - paperChannel * (255 - coverage) + coverage / 2) / coverage, 0, 255);
}

QImage transparentPaperImage(QImage image, const QColor &paperColor)
{
    if (image.format() != QImage::Format_ARGB32) {
        image = image.convertToFormat(QImage::Format_ARGB32);
    }

    for (int y = 0; y < image.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            const int coverage = colorDistance(pixel, paperColor);
            if (coverage <= 0 || qAlpha(pixel) <= 0) {
                line[x] = qRgba(0, 0, 0, 0);
                continue;
            }
            if (coverage >= 255) {
                continue;
            }

            const int alpha = (qAlpha(pixel) * coverage + 127) / 255;
            line[x] = qRgba(unmattedChannel(qRed(pixel), paperColor.red(), coverage),
                            unmattedChannel(qGreen(pixel), paperColor.green(), coverage),
                            unmattedChannel(qBlue(pixel), paperColor.blue(), coverage),
                            alpha);
        }
    }

    return image;
}
}

void PagePainter::paintPageOnPainter(QPainter *destPainter,
                                     const Okular::Page *page,
                                     Okular::DocumentObserver *observer,
                                     int flags,
                                     int scaledWidth,
                                     int scaledHeight,
                                     const QRect limits,
                                     const QColor &foregroundColor,
                                     const QColor &backgroundColor,
                                     const QVector<ColorRegion> &notInvertedRegions,
                                     const QVector<ColorRegion> &forcedInvertedRegions)
{
    if (!destPainter || limits.isEmpty()) {
        return;
    }

    const bool recolor = flags & RecolorForegroundBackground;
    const bool transparentPageBackground = !recolor && (flags & TransparentPageBackground);
    const QColor paperColor = recolor ? backgroundColor : (flags & InvertColors) ? Qt::black : Qt::white;
    if (!transparentPageBackground) {
        destPainter->fillRect(limits, paperColor);
    }
    if (!page || !observer || scaledWidth <= 0 || scaledHeight <= 0) {
        return;
    }

    const qreal dpr = destPainter->device() ? destPainter->device()->devicePixelRatioF() : 1.0;
    const int dScaledWidth = static_cast<int>(std::ceil(scaledWidth * dpr));
    const int dScaledHeight = static_cast<int>(std::ceil(scaledHeight * dpr));
    const QRect dLimits = QRectF(limits.x() * dpr, limits.y() * dpr, limits.width() * dpr, limits.height() * dpr).toAlignedRect();

    const QPixmap *pagePixmap = page->_o_nearestPixmap(observer, dScaledWidth, dScaledHeight);
    if (!pagePixmap || pagePixmap->isNull()) {
        return;
    }

    const QPixmap scaledPixmap = pagePixmap->size() == QSize(dScaledWidth, dScaledHeight)
        ? *pagePixmap
        : pagePixmap->scaled(dScaledWidth, dScaledHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (recolor) {
        QImage image = scaledPixmap.toImage();
        const QVector<ImageColorRegion> preserveRegions = imageRegions(notInvertedRegions, image.size());
        const QVector<ImageColorRegion> forceTransformRegions = imageRegions(forcedInvertedRegions, image.size());
        destPainter->drawImage(limits, recoloredImage(std::move(image), preserveRegions, forceTransformRegions, foregroundColor, backgroundColor), dLimits);
    } else if (flags & InvertColors) {
        QImage image = scaledPixmap.toImage();
        const QVector<ImageColorRegion> preserveRegions = imageRegions(notInvertedRegions, image.size());
        const QVector<ImageColorRegion> forceTransformRegions = imageRegions(forcedInvertedRegions, image.size());
        image = invertedImage(std::move(image), preserveRegions, forceTransformRegions);
        destPainter->drawImage(limits, transparentPageBackground ? transparentPaperImage(std::move(image), paperColor) : image, dLimits);
    } else {
        if (transparentPageBackground) {
            destPainter->drawImage(limits, transparentPaperImage(scaledPixmap.toImage(), paperColor), dLimits);
        } else {
            destPainter->drawPixmap(limits, scaledPixmap, dLimits);
        }
    }

    const Okular::RegularAreaRect *textSelection = page->textSelection();
    if ((flags & TextSelection) && textSelection) {
        QColor selectionColor = page->textSelectionColor();
        if (!selectionColor.isValid()) {
            selectionColor = QColor(0, 96, 192, 96);
        } else if (selectionColor.alpha() == 255) {
            selectionColor.setAlpha(96);
        }

        const QList<QRect> selectionRects = textSelection->geometry(scaledWidth, scaledHeight);
        for (const QRect &selectionRect : selectionRects) {
            const QRect visibleRect = selectionRect.intersected(limits);
            if (!visibleRect.isEmpty()) {
                destPainter->fillRect(visibleRect, selectionColor);
            }
        }
    }
}
