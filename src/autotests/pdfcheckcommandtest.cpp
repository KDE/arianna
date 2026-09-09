// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "booklistmodel.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <podofo/main/PdfMemDocument.h>
#include <podofo/main/PdfPage.h>

using namespace PoDoFo;

namespace
{
void createPdf(const QString &fileName)
{
    PdfMemDocument document;
    document.GetPages().CreatePage(PdfPageSize::A4);
    document.Save(fileName.toStdString(), PdfSaveOptions::NoMetadataUpdate);
}
}

class PdfCheckCommandTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void returnsXmlReportFromPdfCheckCommand();
};

void PdfCheckCommandTest::returnsXmlReportFromPdfCheckCommand()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputFileName = directory.filePath(QStringLiteral("input.pdf"));
    const QString checkerFileName = directory.filePath(QStringLiteral("arlington-pdf-model-checker"));
    createPdf(inputFileName);

    QFile checker(checkerFileName);
    QVERIFY(checker.open(QIODevice::WriteOnly | QIODevice::Text));
    checker.write(
        "#!/bin/sh\n"
        "printf '<report><arlingtonReport isCompliant=\"false\" fileName=\"%s\"/></report>\\n' \"$1\"\n");
    checker.close();
    QVERIFY(checker.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    BookListModel model;
    const QVariantMap result = model.checkPdfFile(inputFileName, checkerFileName);

    QVERIFY(result.value(QStringLiteral("success")).toBool());
    QCOMPARE(result.value(QStringLiteral("exitCode")).toInt(), 0);
    QVERIFY(!result.value(QStringLiteral("message")).toString().isEmpty());
    QVERIFY(result.value(QStringLiteral("output")).toString().contains(QStringLiteral("<arlingtonReport isCompliant=\"false\"")));
    QVERIFY(result.value(QStringLiteral("output")).toString().contains(inputFileName));
    QVERIFY(result.value(QStringLiteral("report")).toMap().value(QStringLiteral("valid")).toBool());
}

QTEST_GUILESS_MAIN(PdfCheckCommandTest)

#include "pdfcheckcommandtest.moc"
