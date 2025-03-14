// SPDX-FileCopyrightText: 2021 Carl Schwan <carlschwan@kde.org>
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <QObject>
#include <QQmlEngine>

class QAbstractItemModel;
class KColorSchemeManager;

/**
 * @class ColorSchemer
 *
 * A class to provide a wrapper around KColorSchemeManager to make it available in
 * QML.
 *
 * @sa KColorSchemeManager
 */
class ColorSchemer : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /**
     * @brief A QAbstractItemModel of all available color schemes.
     *
     * @sa QAbstractItemModel
     */
    Q_PROPERTY(QAbstractItemModel *model READ model CONSTANT)

public:
    explicit ColorSchemer(QObject *parent = nullptr);
    ~ColorSchemer();

    QAbstractItemModel *model() const;

    /**
     * @brief Activates the KColorScheme identified by the provided index.
     *
     * @sa KColorScheme
     */
    Q_INVOKABLE void apply(int idx);

    /**
     * @brief Get the row for the current color scheme.
     *
     * @sa KColorScheme
     */
    Q_INVOKABLE int indexForCurrentScheme();

private:
    void loadSavedScheme();
};
