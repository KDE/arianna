// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pdfsearchprocess.h"

#include <KLocalizedString>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif

namespace
{
QString helperProgram()
{
    // Build-tree binaries live together; installed binaries use KDE's libexec directory.
    const QString besideApplication = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("arianna-pdf-search"));
    if (QFileInfo(besideApplication).isExecutable())
        return besideApplication;
    return QStringLiteral(ARIANNA_PDF_SEARCH_HELPER);
}
}

PdfSearchResult
searchPdfInProcess(const QString &fileName, const QString &query, const std::function<bool()> &cancelled, const PdfSearchProcessOptions &options)
{
    const auto isCancelled = [&] {
        return cancelled && cancelled();
    };
    if (isCancelled())
        return {{}, {}, true};
    if (query.simplified().isEmpty())
        return {};
    QProcess process;
    // PoDoFo can emit thousands of benign xref warnings. Do not buffer or forward them.
    process.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_UNIX
    process.setChildProcessModifier([] {
        const rlimit noCore{0, 0};
        setrlimit(RLIMIT_CORE, &noCore);
        // Only the small text helper is limited; no GUI/WebEngine address space is affected.
        rlimit memory;
        if (getrlimit(RLIMIT_AS, &memory) == 0) {
            constexpr rlim_t limit = 1024ULL * 1024 * 1024;
            memory.rlim_cur = qMin(memory.rlim_cur, limit);
            setrlimit(RLIMIT_AS, &memory);
        }
    });
#endif
    process.start(options.program.isEmpty() ? helperProgram() : options.program, options.arguments, QIODevice::ReadWrite);
    QElapsedTimer time;
    time.start();
    // Send the query over stdin instead of exposing book text in the process arguments.
    const QByteArray request =
        QJsonDocument(QJsonObject{{QStringLiteral("fileName"), fileName}, {QStringLiteral("query"), query.simplified()}}).toJson(QJsonDocument::Compact);
    process.write(request);
    process.closeWriteChannel();
    QByteArray output;
    QString error;
    bool wasCancelled = false;
    for (;;) {
        process.waitForFinished(50);
        const auto chunk = process.readAllStandardOutput();
        if (chunk.size() > options.maximumOutputBytes - output.size()) {
            error = i18n("The PDF search produced too much output. Try a more specific search text.");
            break;
        }
        output += chunk;
        if (isCancelled()) {
            wasCancelled = true;
            break;
        }
        if (process.state() == QProcess::NotRunning)
            break;
        if (time.elapsed() >= options.timeoutMs) {
            error = i18n("The time limit for searching this PDF was exceeded.");
            break;
        }
    }
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(1000);
    }
    if (wasCancelled)
        return {{}, {}, true};
    if (!error.isEmpty())
        return {{}, error};
    if (process.error() == QProcess::FailedToStart)
        return {{}, i18n("Unable to start PDF search: %1", process.errorString())};
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {{}, i18n("Text extraction stopped unexpectedly for this PDF. The remaining books will still be searched.")};
    QJsonParseError parseError;
    const auto response = QJsonDocument::fromJson(output, &parseError);
    const auto object = response.object();
    if (parseError.error != QJsonParseError::NoError || !response.isObject() || object.value(QStringLiteral("version")).toInt() != 1
        || !object.value(QStringLiteral("hits")).isArray() || !object.value(QStringLiteral("error")).isString())
        return {{}, i18n("PDF search returned an invalid result.")};
    QVariantList hits;
    for (const auto &value : object.value(QStringLiteral("hits")).toArray()) {
        if (isCancelled())
            return {{}, {}, true};
        const auto hit = value.toObject();
        for (const auto &key : {"location", "section", "excerpt", "markup"}) {
            if (!hit.value(QLatin1String(key)).isString())
                return {{}, i18n("PDF search returned an invalid result.")};
        }
        hits.append(hit.toVariantMap());
    }
    return {hits, object.value(QStringLiteral("error")).toString()};
}
