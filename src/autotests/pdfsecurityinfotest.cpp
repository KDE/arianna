// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "pdfsecurityinfo.h"

#include <QTemporaryDir>
#include <QTest>

#include <podofo/main/PdfEncrypt.h>
#include <podofo/main/PdfMemDocument.h>
#include <podofo/main/PdfPage.h>

using namespace PoDoFo;

class PdfSecurityInfoTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void reportsUnencryptedPdf();
    void reportsEncryptionRevisionAndPermissions();
};

void PdfSecurityInfoTest::reportsUnencryptedPdf()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileName = directory.filePath(QStringLiteral("plain.pdf"));

    PdfMemDocument document;
    document.GetPages().CreatePage(PdfPageSize::A4);
    document.Save(fileName.toStdString(), PdfSaveOptions::NoMetadataUpdate);

    PdfSecurityInfo securityInfo;
    const QVariantMap result = securityInfo.inspect(fileName);
    QVERIFY(result.value(QStringLiteral("available")).toBool());
    QVERIFY(!result.value(QStringLiteral("encrypted")).toBool());
    QCOMPARE(result.value(QStringLiteral("summary")).toString(), QStringLiteral("The PDF is not encrypted."));
}

void PdfSecurityInfoTest::reportsEncryptionRevisionAndPermissions()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileName = directory.filePath(QStringLiteral("encrypted.pdf"));

    PdfMemDocument document;
    document.GetPages().CreatePage(PdfPageSize::A4);
    // Revision 2 uses the same permission bits as the qpdf example. PoDoFo
    // additionally sets reserved bits in its generated fixture, yielding -3904.
    document.SetEncrypted({}, "owner", PdfPermissions::None, PdfEncryptionAlgorithm::RC4V1, PdfKeyLength::L40);
    document.Save(fileName.toStdString(), PdfSaveOptions::NoMetadataUpdate);

    PdfSecurityInfo securityInfo;
    const QVariantMap result = securityInfo.inspect(fileName);
    QVERIFY(result.value(QStringLiteral("available")).toBool());
    QVERIFY(result.value(QStringLiteral("encrypted")).toBool());
    QCOMPARE(result.value(QStringLiteral("revision")).toInt(), 2);
    QCOMPARE(result.value(QStringLiteral("permissions")).toInt(), -3904);
    QCOMPARE(result.value(QStringLiteral("authenticatedAs")).toString(), QStringLiteral("user"));
    QCOMPARE(result.value(QStringLiteral("algorithm")).toString(), QStringLiteral("RC4"));
    QCOMPARE(result.value(QStringLiteral("keyLength")).toInt(), 40);
    QVERIFY(!result.value(QStringLiteral("accessibility")).toBool());
    QVERIFY(!result.value(QStringLiteral("extract")).toBool());
    QVERIFY(!result.value(QStringLiteral("annotations")).toBool());
    QVERIFY(!result.value(QStringLiteral("anything")).toBool());
    QVERIFY(result.value(QStringLiteral("summary")).toString().contains(QStringLiteral("modify annotations: not allowed")));
}

QTEST_GUILESS_MAIN(PdfSecurityInfoTest)
#include "pdfsecurityinfotest.moc"
