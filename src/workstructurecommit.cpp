// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "workstructurecommit.h"
#include "bookdatabase.h"
#include "booktruthstore.h"
#include "epubcontainer.h"
#include "maintenancefiles.h"
#include "workstructure.h"
#include <KLocalizedString>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <utility>

void WorkStructureCommit::start(QObject *owner,
                                const QString &bookId,
                                const QString &path,
                                const QJsonObject &snap,
                                const QJsonObject &draft,
                                const QString &expectedRevision,
                                const QString &validator,
                                std::function<void(const QJsonObject &)> completed)
{
    auto fail = [&](const QString &message) {
        completed({{QStringLiteral("success"), false}, {QStringLiteral("message"), message}});
    };
    EPubContainer original(nullptr);
    if (!original.openFile(path) || original.getFileHash() != snap.value(QStringLiteral("sourceFileHash")).toString()) {
        fail(i18n("The EPUB changed during review. Analyze it again before writing the structure."));
        return;
    }
    const auto existing = WorkStructure::read(original, snap);
    const auto oldRecord = existing.value(QStringLiteral("record")).toObject();
    if (oldRecord.value(QStringLiteral("revision")).toString() != expectedRevision) {
        fail(i18n("The confirmed structure changed during review."));
        return;
    }
    QString error;
    const auto record = WorkStructure::confirmedRecord(snap, draft, &error);
    if (record.isEmpty()) {
        fail(error);
        return;
    }
    if (existing.value(QStringLiteral("validity")) == QJsonValue(QStringLiteral("valid"))
        && oldRecord.value(QStringLiteral("levels")) == record.value(QStringLiteral("levels"))
        && oldRecord.value(QStringLiteral("nodes")) == record.value(QStringLiteral("nodes"))
        && oldRecord.value(QStringLiteral("coordinateFormats")) == record.value(QStringLiteral("coordinateFormats"))) {
        completed({{QStringLiteral("success"), true},
                   {QStringLiteral("unchanged"), true},
                   {QStringLiteral("bookId"), bookId},
                   {QStringLiteral("record"), oldRecord}});
        return;
    }
    const auto bytes = WorkStructure::candidate(original, record, &error);
    if (bytes.isEmpty()) {
        fail(error);
        return;
    }
    auto directory = std::make_shared<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/arianna-structure-XXXXXX"));
    if (!directory->isValid()) {
        fail(i18n("Unable to create the EPUB validation working copy."));
        return;
    }
    const QString candidatePath = directory->filePath(QStringLiteral("candidate.epub"));
    QFile output(candidatePath);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size()) {
        fail(i18n("Unable to write the EPUB validation working copy."));
        return;
    }
    output.close();
    EPubContainer candidate(nullptr);
    if (!candidate.openFile(candidatePath) || !WorkStructure::preservesSource(original, candidate, &error)
        || WorkStructure::snapshot(candidate, bookId).value(QStringLiteral("existing")).toObject().value(QStringLiteral("validity"))
            != QJsonValue(QStringLiteral("valid"))) {
        fail(error.isEmpty() ? i18n("The exported structure could not be verified.") : error);
        return;
    }
    QStringList command = QProcess::splitCommand(validator);
    // Debian-family installations ship the validator as a JAR without a PATH
    // wrapper. Resolve only the conventional bare command; custom commands and
    // explicit empty values keep their configured meaning.
    if (command == QStringList{QStringLiteral("epubcheck")} && QStandardPaths::findExecutable(command.first()).isEmpty()
        && QFile::exists(QStringLiteral("/usr/share/java/epubcheck.jar")) && !QStandardPaths::findExecutable(QStringLiteral("java")).isEmpty()) {
        command = {QStringLiteral("java"), QStringLiteral("-jar"), QStringLiteral("/usr/share/java/epubcheck.jar")};
    }
    if (command.isEmpty()) {
        fail(i18n("Configure EPUBCheck before writing the structure."));
        return;
    }
    auto process = new QProcess(owner);
    auto timer = new QTimer(process);
    timer->setSingleShot(true);
    auto done = std::make_shared<bool>(false);
    auto log = std::make_shared<QByteArray>();
    auto finish = [process, done, completed, directory](QJsonObject result) {
        if (std::exchange(*done, true))
            return;
        completed(result);
        process->deleteLater();
    };
    process->setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(process, &QProcess::readyReadStandardOutput, process, [process, log] {
        const auto bytes = process->readAllStandardOutput();
        if (log->size() < 256 * 1024)
            log->append(bytes.left(256 * 1024 - log->size()));
    });
    QObject::connect(timer, &QTimer::timeout, process, [process] {
        process->setProperty("timedOut", true);
        process->kill();
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish({{QStringLiteral("success"), false}, {QStringLiteral("message"), i18n("EPUBCheck could not be started. The EPUB was not changed.")}});
    });
    QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process, [=](int code, QProcess::ExitStatus status) {
        timer->stop();
        log->append(process->readAllStandardOutput().left(256 * 1024 - log->size()));
        if (status != QProcess::NormalExit || code != 0 || process->property("timedOut").toBool()) {
            finish({{QStringLiteral("success"), false},
                    {QStringLiteral("message"), i18n("EPUBCheck rejected the candidate. The EPUB was not changed.")},
                    {QStringLiteral("validatorOutput"), QString::fromUtf8(*log)}});
            return;
        }
        const auto entry = BookDatabase::self().loadEntryByUniqueIdentifier(bookId);
        auto guard = MaintenanceFiles::lock();
        if (!entry || !guard) {
            finish({{QStringLiteral("success"), false},
                    {QStringLiteral("message"), i18n("Editor files are currently being checked or changed. Please try again.")}});
            return;
        }
        const auto blocked = MaintenanceFiles::anchorEditBlockReason(*entry);
        if (!blocked.isEmpty()) {
            finish({{QStringLiteral("success"), false}, {QStringLiteral("message"), blocked}});
            return;
        }
        const auto committed = BookTruthStore().commitCandidateBookFile(bookId,
                                                                        candidatePath,
                                                                        false,
                                                                        true,
                                                                        snap.value(QStringLiteral("sourceFileHash")).toString().toLatin1(),
                                                                        true);
        finish({{QStringLiteral("success"), committed.success},
                {QStringLiteral("message"), committed.errorMessage},
                {QStringLiteral("unchanged"), committed.unchanged},
                {QStringLiteral("bookId"), bookId},
                {QStringLiteral("record"), record},
                {QStringLiteral("newStateId"), committed.newStateId.toString(QUuid::WithoutBraces)}});
    });
    QStringList args = command.mid(1);
    args.append(candidatePath);
    process->start(command.first(), args);
    timer->start(120000);
}
