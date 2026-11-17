// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include <optional>

namespace Transliterator
{
bool containsDevanagari(const QString &text);
// A missing value indicates a conversion failure. The input is never modified.
std::optional<QString> devanagariToIast(const QString &text);
std::optional<QString> iastToDevanagari(const QString &text);
}
