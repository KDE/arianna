// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bookcursor.h"

#include <QPixmap>
#include <QQuickWindow>

BookCursor::BookCursor(QQuickItem *item)
    : QObject(item)
    , m_item(item)
{
    const auto updateWindow = [this](QQuickWindow *window) {
        QObject::disconnect(m_screenChangedConnection);
        if (window) {
            m_screenChangedConnection = connect(window, &QWindow::screenChanged, this, &BookCursor::updateCursor);
        }
        updateCursor();
    };
    connect(item, &QQuickItem::windowChanged, this, updateWindow);
    updateWindow(item->window());
}

BookCursor *BookCursor::qmlAttachedProperties(QObject *object)
{
    auto *item = qobject_cast<QQuickItem *>(object);
    return item ? new BookCursor(item) : nullptr;
}

bool BookCursor::active() const
{
    return m_active;
}

void BookCursor::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    updateCursor();
    Q_EMIT activeChanged();
}

Qt::CursorShape BookCursor::fallbackShape() const
{
    return m_fallbackShape;
}

void BookCursor::setFallbackShape(Qt::CursorShape shape)
{
    if (m_fallbackShape == shape) {
        return;
    }
    m_fallbackShape = shape;
    updateCursor();
    Q_EMIT fallbackShapeChanged();
}

void BookCursor::updateCursor()
{
    if (!m_active) {
        m_item->setCursor(m_fallbackShape);
        return;
    }

    const qreal devicePixelRatio = m_item->window() ? m_item->window()->devicePixelRatio() : 1;
    if (!qFuzzyCompare(devicePixelRatio, m_cursorDevicePixelRatio)) {
        const QPixmap icon(QStringLiteral(":/qt/qml/org/kde/arianna/qml/icons/book.png"));
        if (icon.isNull()) {
            m_item->setCursor(Qt::PointingHandCursor);
            return;
        }
        const int pixelSize = qRound(32 * devicePixelRatio);
        QPixmap pixmap = icon.scaled(pixelSize, pixelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pixmap.setDevicePixelRatio(devicePixelRatio);
        m_bookCursor = QCursor(pixmap);
        m_cursorDevicePixelRatio = devicePixelRatio;
    }
    m_item->setCursor(m_bookCursor);
}
