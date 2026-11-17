// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>
#include <functional>

/// Exported on the existing per-process KDBusService, at /Reader.
class ReaderControl : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.arianna.Reader1")
public:
    explicit ReaderControl(std::function<int()> refresh, QObject *parent = nullptr);

public Q_SLOTS:
    // >= 0: number of refreshed readers; -1: failed/invalid, -2: already busy.
    int RefreshReaders(const QString &requestId);

private:
    std::function<int()> m_refresh;
    QHash<QString, int> m_completed;
    QStringList m_requestOrder;
    bool m_refreshing = false;
};
