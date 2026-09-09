// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QString>

struct PdfVersionUpdateResult {
    bool updated = false;
    bool alreadyVersion20 = false;
    QString errorString;
};

class PdfVersionUpdater
{
public:
    static PdfVersionUpdateResult setVersion20(const QString &inputFileName, const QString &outputFileName);
};
