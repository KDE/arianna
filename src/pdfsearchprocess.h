// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QStringList>
#include <QVariantList>
#include <functional>

struct PdfSearchResult {
    QVariantList hits;
    QString error;
    bool cancelled = false;
};

struct PdfSearchProcessOptions {
    // Empty selects the bundled helper. Overrides also allow deterministic failure tests.
    QString program;
    QStringList arguments;
    int timeoutMs = 120000;
    qsizetype maximumOutputBytes = 16 * 1024 * 1024;
};

/// Runs PDF parsing outside the application: SIGSEGV/abort cannot be caught by C++ exceptions.
/// Synchronous on the caller's worker thread, with bounded output, time and cancellation polling.
PdfSearchResult
searchPdfInProcess(const QString &fileName, const QString &query, const std::function<bool()> &cancelled = {}, const PdfSearchProcessOptions &options = {});
