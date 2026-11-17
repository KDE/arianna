// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QCursor>
#include <QObject>
#include <QQuickItem>
#include <qqmlintegration.h>

// MouseArea exposes only standard cursor shapes. This attached property sets
// a native bitmap cursor on the item while keeping its normal cursor local.
class BookCursor : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_ATTACHED(BookCursor)
    QML_UNCREATABLE("Attached property for a Quick item")

    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(Qt::CursorShape fallbackShape READ fallbackShape WRITE setFallbackShape NOTIFY fallbackShapeChanged)

public:
    explicit BookCursor(QQuickItem *item);
    static BookCursor *qmlAttachedProperties(QObject *object);

    bool active() const;
    void setActive(bool active);
    Qt::CursorShape fallbackShape() const;
    void setFallbackShape(Qt::CursorShape shape);

Q_SIGNALS:
    void activeChanged();
    void fallbackShapeChanged();

private:
    void updateCursor();

    QQuickItem *const m_item;
    bool m_active = false;
    Qt::CursorShape m_fallbackShape = Qt::ArrowCursor;
    QCursor m_bookCursor;
    qreal m_cursorDevicePixelRatio = 0;
    QMetaObject::Connection m_screenChangedConnection;
};
