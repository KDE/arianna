// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "categoryentriesmodel.h"

namespace BookFormats
{
QString type(const BookEntry &book);
QStringList types(const BookEntry &book);
QString identity(const BookEntry &book);
QString displayKey(const BookEntry &book);
QVariantList candidates(const QList<BookEntry> &books);
QList<BookEntry> members(const BookEntry &book);
BookEntry combined(const QList<BookEntry> &books);
}
