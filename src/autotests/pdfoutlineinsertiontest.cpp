// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "okularpdfdocument.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <string_view>

#include <podofo/main/PdfDestination.h>
#include <podofo/main/PdfMemDocument.h>
#include <podofo/main/PdfOutlines.h>
#include <podofo/main/PdfPage.h>

using namespace PoDoFo;

namespace
{
QString pdfLocationForPage(int page)
{
    QJsonObject location;
    location.insert(QStringLiteral("format"), QStringLiteral("pdf"));
    location.insert(QStringLiteral("page"), page);
    return QString::fromUtf8(QJsonDocument(location).toJson(QJsonDocument::Compact));
}

void setDestination(PdfMemDocument &document, PdfOutlineItem &item, int page)
{
    auto destination = document.CreateDestination();
    destination->SetDestination(document.GetPages().GetPageAt(static_cast<unsigned>(page)));
    item.SetDestination(*destination);
}

QJsonObject insertion(const QString &title, int page, const QString &placement, const QString &targetId)
{
    QJsonObject entry;
    entry.insert(QStringLiteral("label"), title);
    entry.insert(QStringLiteral("href"), pdfLocationForPage(page));
    entry.insert(QStringLiteral("placement"), placement);
    entry.insert(QStringLiteral("targetId"), targetId);
    entry.insert(QStringLiteral("subitems"), QJsonArray());
    return entry;
}

QString outlineTitle(PdfOutlineItem *item)
{
    const std::string_view title = item->GetTitle().GetString();
    return QString::fromUtf8(title.data(), static_cast<qsizetype>(title.size()));
}

QStringList outlineTitles(PdfOutlineItem *first)
{
    QStringList titles;
    for (PdfOutlineItem *item = first; item; item = item->Next()) {
        titles.append(outlineTitle(item));
    }
    return titles;
}
}

class PdfOutlineInsertionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void insertsBeforeAfterAndBelowExistingItems();
};

void PdfOutlineInsertionTest::insertsBeforeAfterAndBelowExistingItems()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileName = directory.filePath(QStringLiteral("outline.pdf"));

    PdfMemDocument pdf;
    for (int page = 0; page < 6; ++page) {
        pdf.GetPages().CreatePage(PdfPageSize::A4);
    }

    PdfOutlines &outlines = pdf.GetOrCreateOutlines();
    PdfOutlineItem &first = outlines.CreateChild(PdfString("First"));
    PdfOutlineItem &second = first.CreateNext(PdfString("Second"));
    PdfOutlineItem &third = second.CreateNext(PdfString("Third"));
    setDestination(pdf, first, 0);
    setDestination(pdf, second, 1);
    setDestination(pdf, third, 2);
    pdf.Save(fileName.toStdString());

    QJsonArray insertions;
    insertions.append(insertion(QStringLiteral("Before First"), 3, QStringLiteral("before"), QStringLiteral("pdf-toc-1")));
    insertions.append(insertion(QStringLiteral("After Second"), 4, QStringLiteral("after"), QStringLiteral("pdf-toc-2")));
    insertions.append(insertion(QStringLiteral("Child of Third"), 5, QStringLiteral("child"), QStringLiteral("pdf-toc-3")));

    OkularPdfDocument document;
    const QVariantMap result = document.writeTableOfContentsToPdf(QString::fromUtf8(QJsonDocument(insertions).toJson(QJsonDocument::Compact)), fileName);
    QVERIFY(result.value(QStringLiteral("success")).toBool());

    PdfMemDocument updatedPdf;
    updatedPdf.Load(fileName.toStdString());
    PdfOutlines *updatedOutlines = updatedPdf.GetOutlines();
    QVERIFY(updatedOutlines);
    QCOMPARE(outlineTitles(updatedOutlines->First()),
             QStringList(
                 {QStringLiteral("Before First"), QStringLiteral("First"), QStringLiteral("Second"), QStringLiteral("After Second"), QStringLiteral("Third")}));

    PdfOutlineItem *thirdItem = updatedOutlines->First();
    while (thirdItem && outlineTitle(thirdItem) != QStringLiteral("Third")) {
        thirdItem = thirdItem->Next();
    }
    QVERIFY(thirdItem);
    QCOMPARE(outlineTitles(thirdItem->First()), QStringList({QStringLiteral("Child of Third")}));

    const QVariantMap editResult = document.editTableOfContentsItemInPdf(QStringLiteral("pdf-toc-2"), QStringLiteral("Renamed First"), fileName);
    QVERIFY(editResult.value(QStringLiteral("success")).toBool());

    updatedPdf.Load(fileName.toStdString());
    updatedOutlines = updatedPdf.GetOutlines();
    QVERIFY(updatedOutlines);
    QCOMPARE(outlineTitles(updatedOutlines->First()),
             QStringList({QStringLiteral("Before First"),
                          QStringLiteral("Renamed First"),
                          QStringLiteral("Second"),
                          QStringLiteral("After Second"),
                          QStringLiteral("Third")}));

    const QVariantMap removeResult = document.removeTableOfContentsItemFromPdf(QStringLiteral("pdf-toc-6"), fileName);
    QVERIFY(removeResult.value(QStringLiteral("success")).toBool());

    updatedPdf.Load(fileName.toStdString());
    updatedOutlines = updatedPdf.GetOutlines();
    QVERIFY(updatedOutlines);
    QCOMPARE(outlineTitles(updatedOutlines->First()),
             QStringList({QStringLiteral("Before First"), QStringLiteral("Renamed First"), QStringLiteral("Second"), QStringLiteral("After Second")}));
}

QTEST_MAIN(PdfOutlineInsertionTest)
#include "pdfoutlineinsertiontest.moc"
