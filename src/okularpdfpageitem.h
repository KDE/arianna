// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "okularpdfpagepainter.h"

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <qqmlintegration.h>

#include <memory>

#include <core/view.h>

class QMouseEvent;
class QHoverEvent;
class QTimer;
class OkularPdfDocument;

namespace Okular
{
class Action;
class NormalizedPoint;
class Page;
class RegularAreaRect;
}

class OkularPdfPageItem : public QQuickItem, public Okular::View
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(OkularPdfDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(int pageNumber READ pageNumber WRITE setPageNumber NOTIFY pageNumberChanged)
    Q_PROPERTY(bool pageValid READ pageValid NOTIFY pageValidChanged)
    Q_PROPERTY(bool inverted READ inverted WRITE setInverted NOTIFY invertedChanged)
    Q_PROPERTY(bool recolor READ recolor WRITE setRecolor NOTIFY recolorChanged)
    Q_PROPERTY(bool transparentPageBackground READ transparentPageBackground WRITE setTransparentPageBackground NOTIFY transparentPageBackgroundChanged)
    Q_PROPERTY(QColor foregroundColor READ foregroundColor WRITE setForegroundColor NOTIFY foregroundColorChanged)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor NOTIFY backgroundColorChanged)
    Q_PROPERTY(QVariantList notInvertedRegions READ notInvertedRegions WRITE setNotInvertedRegions NOTIFY notInvertedRegionsChanged)
    Q_PROPERTY(QVariantList forcedInvertedRegions READ forcedInvertedRegions WRITE setForcedInvertedRegions NOTIFY forcedInvertedRegionsChanged)

public:
    explicit OkularPdfPageItem(QQuickItem *parent = nullptr);
    ~OkularPdfPageItem() override;

    OkularPdfDocument *document() const;
    void setDocument(OkularPdfDocument *document);

    int pageNumber() const;
    void setPageNumber(int pageNumber);

    bool pageValid() const;
    bool inverted() const;
    void setInverted(bool inverted);
    bool recolor() const;
    void setRecolor(bool recolor);
    bool transparentPageBackground() const;
    void setTransparentPageBackground(bool transparentPageBackground);
    QColor foregroundColor() const;
    void setForegroundColor(const QColor &foregroundColor);
    QColor backgroundColor() const;
    void setBackgroundColor(const QColor &backgroundColor);
    QVariantList notInvertedRegions() const;
    void setNotInvertedRegions(const QVariantList &regions);
    QVariantList forcedInvertedRegions() const;
    void setForcedInvertedRegions(const QVariantList &regions);
    Q_INVOKABLE QVariantMap refinedColorRegion(const QVariant &region) const;
    Q_INVOKABLE bool hasLinkAt(qreal x, qreal y) const;

    QSGNode *updatePaintNode(QSGNode *node, QQuickItem::UpdatePaintNodeData *data) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

Q_SIGNALS:
    void documentChanged();
    void pageNumberChanged();
    void pageValidChanged();
    void invertedChanged();
    void recolorChanged();
    void transparentPageBackgroundChanged();
    void foregroundColorChanged();
    void backgroundColorChanged();
    void notInvertedRegionsChanged();
    void forcedInvertedRegionsChanged();
    void selectionMenuRequested(qreal x, qreal y);
    void pageColorMenuRequested(qreal x, qreal y);
    void linkActivated(const QString &location, const QString &sourceLocation);
    void externalLinkActivated(const QString &url, const QString &sourceLocation);

protected:
    void hoverEnterEvent(QHoverEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void refreshPage();
    void schedulePixmapRequest();
    void requestPixmap();
    void paint();
    void clearBuffer();
    void handlePageChanged(int page, int flags);
    bool canSelectText() const;
    void updateCursorForPosition(const QPointF &position);
    const Okular::Action *linkActionAt(const QPointF &position) const;
    const Okular::Action *internalLinkActionAt(const QPointF &position) const;
    bool activateLinkAction(const Okular::Action *action, const QPointF &sourcePosition);
    void updateTextSelectionOverlay();
    void updateTextSelection(const QPointF &endPosition);
    std::unique_ptr<Okular::RegularAreaRect> textSelectionForPoints(const QPointF &startPosition, const QPointF &endPosition) const;
    Okular::NormalizedPoint normalizedPointForPosition(const QPointF &position) const;

    QPointer<OkularPdfDocument> m_document;
    const Okular::Page *m_page = nullptr;
    QTimer *m_redrawTimer = nullptr;
    QImage m_buffer;
    QVector<QRectF> m_textSelectionRects;
    QColor m_textSelectionColor = QColor(0, 96, 192, 96);
    int m_pageNumber = -1;
    bool m_pageValid = false;
    bool m_textureDirty = false;
    bool m_inverted = false;
    bool m_recolor = false;
    bool m_transparentPageBackground = false;
    QColor m_foregroundColor = Qt::black;
    QColor m_backgroundColor = Qt::white;
    QVariantList m_notInvertedRegions;
    QVector<PagePainter::ColorRegion> m_notInvertedRegionRects;
    QVariantList m_forcedInvertedRegions;
    QVector<PagePainter::ColorRegion> m_forcedInvertedRegionRects;
    QPointF m_selectionStartPosition;
    QPoint m_lastSelectionEndPosition;
    const Okular::Action *m_pressedLinkAction = nullptr;
    bool m_mousePressed = false;
    bool m_textSelecting = false;
    bool m_hasLastSelectionEndPosition = false;
};
