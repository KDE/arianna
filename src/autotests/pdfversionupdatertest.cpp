// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "pdfversionupdater.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <podofo/main/PdfMemDocument.h>
#include <podofo/main/PdfPage.h>

using namespace PoDoFo;

namespace
{
QString pdfHeaderVersion(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    const QByteArray header = file.read(1024);
    const qsizetype markerIndex = header.indexOf("%PDF-");
    if (markerIndex < 0) {
        return {};
    }

    const qsizetype versionStart = markerIndex + 5;
    qsizetype versionEnd = versionStart;
    while (versionEnd < header.size()) {
        const char character = header.at(versionEnd);
        if ((character < '0' || character > '9') && character != '.') {
            break;
        }
        ++versionEnd;
    }

    return QString::fromLatin1(header.mid(versionStart, versionEnd - versionStart)).trimmed();
}

void createPdf(const QString &fileName, PdfVersion version)
{
    PdfMemDocument document;
    document.GetPages().CreatePage(PdfPageSize::A4);
    document.GetMetadata().SetPdfVersion(version);
    document.Save(fileName.toStdString(), PdfSaveOptions::NoMetadataUpdate);
}
}

class PdfVersionUpdaterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void setsPdfHeaderToVersion20();
    void leavesVersion20PdfUntouched();
};

void PdfVersionUpdaterTest::setsPdfHeaderToVersion20()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputFileName = directory.filePath(QStringLiteral("input.pdf"));
    const QString outputFileName = directory.filePath(QStringLiteral("output.pdf"));
    createPdf(inputFileName, PdfVersion::V1_7);

    QCOMPARE(pdfHeaderVersion(inputFileName), QStringLiteral("1.7"));
    const PdfVersionUpdateResult result = PdfVersionUpdater::setVersion20(inputFileName, outputFileName);

    QVERIFY(result.updated);
    QVERIFY(!result.alreadyVersion20);
    QVERIFY(result.errorString.isEmpty());
    QCOMPARE(pdfHeaderVersion(inputFileName), QStringLiteral("1.7"));
    QCOMPARE(pdfHeaderVersion(outputFileName), QStringLiteral("2.0"));

    PdfMemDocument document;
    document.Load(outputFileName.toStdString());
    QCOMPARE(document.GetMetadata().GetPdfVersion(), PdfVersion::V2_0);
}

void PdfVersionUpdaterTest::leavesVersion20PdfUntouched()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputFileName = directory.filePath(QStringLiteral("input.pdf"));
    const QString outputFileName = directory.filePath(QStringLiteral("output.pdf"));
    createPdf(inputFileName, PdfVersion::V2_0);

    const PdfVersionUpdateResult result = PdfVersionUpdater::setVersion20(inputFileName, outputFileName);

    QVERIFY(!result.updated);
    QVERIFY(result.alreadyVersion20);
    QVERIFY(result.errorString.isEmpty());
    QVERIFY(!QFileInfo::exists(outputFileName));
}

QTEST_GUILESS_MAIN(PdfVersionUpdaterTest)

#include "pdfversionupdatertest.moc"
