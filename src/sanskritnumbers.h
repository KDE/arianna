// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QString>
#include <QStringView>
#include <QVariantList>

namespace SanskritNumbers
{
// Expand standalone ASCII/Devanagari integers 0..100 for speech only.
// A source slice retains the context of the whole segment, so marks inside
// decimal numbers, references or a numeral never cause partial conversion.
QString coordinateForSpeech(const QVariantList &semanticPath);
QString expandForSpeech(QStringView text, qsizetype start = 0, qsizetype length = -1);
}
