// SPDX-License-Identifier: GPL-2.0-or-later
#include "readerhistory.h"

#include "bookdatabase.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <algorithm>
#include <utility>

namespace
{
// Preserve event order even when several pages open/close in one millisecond.
QDateTime nextTime(QDateTime &previous)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    previous = previous.isValid() && now <= previous ? previous.addMSecs(1) : now;
    return previous;
}
}

ReaderHistory::ReaderHistory(QObject *parent)
    : QObject(parent)
{
    connect(qApp, &QCoreApplication::aboutToQuit, this, &ReaderHistory::closeReaders);
}

ReaderHistory::~ReaderHistory()
{
    closeReaders();
}

void ReaderHistory::trackReader(QObject *reader, const QString &fileName)
{
    if (!reader || fileName.isEmpty()) {
        return;
    }
    const auto existing = m_readers.find(reader);
    if (existing != m_readers.end()) {
        existing->fileName = fileName;
        return;
    }
    m_readers.insert(reader, {fileName, nextTime(m_lastOpened)});
    connect(reader, &QObject::destroyed, this, [this, reader] { closeReader(reader); });
}

void ReaderHistory::saveClosed(const Session &session)
{
    BookDatabase::self().recordReaderClosed(session.fileName, session.opened, nextTime(m_lastClosed));
}

void ReaderHistory::closeReader(QObject *reader)
{
    const auto found = m_readers.find(reader);
    if (found == m_readers.end()) {
        return;
    }
    const auto session = *found;
    m_readers.erase(found);
    saveClosed(session);
}

void ReaderHistory::closeReaders()
{
    auto sessions = m_readers.values();
    m_readers.clear();
    std::sort(sessions.begin(), sessions.end(), [](const Session &a, const Session &b) { return a.opened < b.opened; });
    for (const auto &session : std::as_const(sessions)) {
        saveClosed(session);
    }
}

QList<BookEntry> ReaderHistory::lastClosedBooks(int count)
{
    if (count <= 0) {
        return {};
    }
    auto entries = BookDatabase::self().loadEntries();
    entries.removeIf([](const BookEntry &entry) {
        return !entry.lastClosedTime.isValid() || !entry.lastClosedOpenedTime.isValid() || !QFileInfo(entry.filename).isFile();
    });
    std::sort(entries.begin(), entries.end(), [](const BookEntry &a, const BookEntry &b) {
        if (a.lastClosedTime != b.lastClosedTime) return a.lastClosedTime > b.lastClosedTime;
        if (a.lastClosedOpenedTime != b.lastClosedOpenedTime) return a.lastClosedOpenedTime > b.lastClosedOpenedTime;
        return a.filename < b.filename;
    });
    if (entries.size() > count) {
        entries.resize(count);
    }
    std::sort(entries.begin(), entries.end(), [](const BookEntry &a, const BookEntry &b) {
        if (a.lastClosedOpenedTime != b.lastClosedOpenedTime) return a.lastClosedOpenedTime < b.lastClosedOpenedTime;
        return a.filename < b.filename;
    });
    return entries;
}
