// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QRectF>
#include <QVector>

class QColor;
class QPainter;
class QRect;

namespace Okular
{
class DocumentObserver;
class Page;
}

class PagePainter
{
public:
    struct ColorRegion {
        QRectF rect;
        bool elliptical = false;
    };

    enum PagePainterFlags {
        TextSelection = 16,
        InvertColors = 32,
        RecolorForegroundBackground = 64,
        TransparentPageBackground = 128,
    };

    static void paintPageOnPainter(QPainter *destPainter,
                                   const Okular::Page *page,
                                   Okular::DocumentObserver *observer,
                                   int flags,
                                   int scaledWidth,
                                   int scaledHeight,
                                   const QRect limits,
                                   const QColor &foregroundColor,
                                   const QColor &backgroundColor,
                                   const QVector<ColorRegion> &notInvertedRegions,
                                   const QVector<ColorRegion> &forcedInvertedRegions);
};
