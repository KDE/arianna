// SPDX-FileCopyrightText: 2012 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QObject>
#include <QVariant>

/**
 * The only purpose of this class is to expose the dynamic property
 * system of Qt to QML, so we can set and get properties on a generic
 * object. It is a little bit of a hack, but QML deliberately does
 * not have access to this (according to the developers).
 */
class PropertyContainer : public QObject
{
    Q_OBJECT
public:
    explicit PropertyContainer(QObject *parent = nullptr);
    explicit PropertyContainer(const QString &name, QObject *parent = nullptr);
    ~PropertyContainer() override;

    // As QObject already as setProperty and property() functions, we must
    // name ours differently
    Q_INVOKABLE void writeProperty(const QString &name, const QVariant &value);
    Q_INVOKABLE QVariant readProperty(const QString &name);

    Q_INVOKABLE QString name() const;

private:
    QString m_name;
};
