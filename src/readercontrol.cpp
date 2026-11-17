// SPDX-License-Identifier: GPL-2.0-or-later
#include "readercontrol.h"

#include <QScopedValueRollback>
#include <QUuid>
#include <utility>

ReaderControl::ReaderControl(std::function<int()> refresh, QObject *parent)
    : QObject(parent)
    , m_refresh(std::move(refresh))
{
}

int ReaderControl::RefreshReaders(const QString &requestId)
{
    if (QUuid(requestId).isNull()) return -1;
    if (m_completed.contains(requestId)) return m_completed.value(requestId);
    if (m_refreshing) return -2;
    QScopedValueRollback<bool> guard(m_refreshing, true);
    const int result = m_refresh();
    m_completed.insert(requestId, result);
    m_requestOrder.append(requestId);
    if (m_requestOrder.size() > 128) m_completed.remove(m_requestOrder.takeFirst());
    return result;
}
