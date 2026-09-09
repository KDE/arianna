// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QString>

struct PdfWatermarkFilterResult {
    bool attempted = false;
    bool filtered = false;
    int occurrences = 0;
    QString errorString;
};

class PdfWatermarkFilter
{
public:
    static PdfWatermarkFilterResult filterFile(const QString &inputFileName, const QString &outputFileName, const QString &pattern);
};
