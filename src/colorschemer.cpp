// SPDX-FileCopyrightText: 2021 Carl Schwan <carlschwan@kde.org>
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "colorschemer.h"
#include <KColorSchemeManager>
#include <KConfigGroup>
#include <KSharedConfig>
#include <QAbstractItemModel>
#include <QQmlEngine>

ColorSchemer::ColorSchemer(QObject *parent)
    : QObject(parent)
{
    qmlRegisterSingletonInstance("org.kde.arianna", 1, 0, "ColorSchemer", this);
    loadSavedScheme();
}

ColorSchemer::~ColorSchemer()
{
}

QAbstractItemModel *ColorSchemer::model() const
{
    return KColorSchemeManager::instance()->model();
}

void ColorSchemer::apply(int idx)
{
    KColorSchemeManager::instance()->activateScheme(KColorSchemeManager::instance()->model()->index(idx, 0));
    auto config = KSharedConfig::openConfig();
    KConfigGroup group(config, QStringLiteral("UiSettings"));
    group.writeEntry("ColorScheme", KColorSchemeManager::instance()->activeSchemeId());
    group.sync();
}

int ColorSchemer::indexForCurrentScheme()
{
    return KColorSchemeManager::instance()->indexForSchemeId(KColorSchemeManager::instance()->activeSchemeId()).row();
}

void ColorSchemer::loadSavedScheme()
{
    auto config = KSharedConfig::openConfig();
    KConfigGroup group(config, QStringLiteral("UiSettings"));
    QString savedScheme = group.readEntry("ColorScheme", QString());

    if (!savedScheme.isEmpty()) {
        int idx = KColorSchemeManager::instance()->indexForSchemeId(savedScheme).row();
        if (idx >= 0) {
            apply(idx);
        }
    }
}

#include "moc_colorschemer.cpp"
