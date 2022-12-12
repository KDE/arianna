// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.0-or-later

#include "format.h"
#include <kformat.h>

QString Format::formatDuration(quint64 milliseconds) const
{
    KFormat format;
    KFormat::DurationFormatOptions options = static_cast<KFormat::DurationFormatOptions>(KFormat::InitialDuration | KFormat::HideSeconds);
    return format.formatDuration(milliseconds, options);
}
