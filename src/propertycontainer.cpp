// SPDX-FileCopyrightText: 2012 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "propertycontainer.h"

PropertyContainer::PropertyContainer::PropertyContainer(QObject *parent)
    : QObject(parent)
    , m_name(QStringLiteral("unnamed"))
{
}

PropertyContainer::PropertyContainer(const QString &name, QObject *parent)
    : QObject(parent)
    , m_name(name)
{
}

PropertyContainer::~PropertyContainer() = default;

void PropertyContainer::writeProperty(const QString &name, const QVariant &value)
{
    setProperty(name.toLatin1().toStdString().c_str(), value);
}

QVariant PropertyContainer::readProperty(const QString &name)
{
    return property(name.toLatin1().toStdString().c_str());
}

QString PropertyContainer::name() const
{
    return m_name;
}

#include "moc_propertycontainer.cpp"
