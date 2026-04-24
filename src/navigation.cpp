// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "navigation.h"

#include <QUuid>

Navigation::Navigation(QObject *parent)
    : QObject(parent)
    , m_bookServerToken(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

QString Navigation::bookServerToken() const
{
    return m_bookServerToken;
}

#include "moc_navigation.cpp"
