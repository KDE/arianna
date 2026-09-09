// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "pdfwatermarkfilter.h"

#include <QByteArray>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QVector>

#include <KLocalizedString>

#include <podofo/main/PdfMemDocument.h>
#include <podofo/main/PdfObjectStream.h>
#include <podofo/main/PdfPage.h>
#include <podofo/main/PdfResources.h>

#include <optional>
#include <string>

namespace
{
struct WatermarkMarker {
    std::string privateKey;
    std::string value;
};

enum class ContentTokenType {
    Name,
    Keyword,
    Other,
};

struct ContentToken {
    ContentTokenType type = ContentTokenType::Other;
    qsizetype begin = 0;
    qsizetype end = 0;
    QByteArray value;
};

struct RemovedWatermarkInvocations {
    int count = 0;
    QSet<QByteArray> resourceNames;
};

struct PageResourceCleanup {
    PoDoFo::PdfResources *resources = nullptr;
    QSet<QByteArray> resourceNames;
};

bool isPdfWhitespace(char character)
{
    switch (character) {
    case '\0':
    case '\t':
    case '\n':
    case '\f':
    case '\r':
    case ' ':
        return true;
    default:
        return false;
    }
}

bool isPdfDelimiter(char character)
{
    switch (character) {
    case '(':
    case ')':
    case '<':
    case '>':
    case '[':
    case ']':
    case '{':
    case '}':
    case '/':
    case '%':
        return true;
    default:
        return false;
    }
}

int hexDigitValue(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return -1;
}

QByteArray decodePdfName(const QByteArray &encodedName)
{
    QByteArray decodedName;
    decodedName.reserve(encodedName.size());

    for (qsizetype index = 0; index < encodedName.size(); ++index) {
        if (encodedName.at(index) == '#' && index + 2 < encodedName.size()) {
            const int high = hexDigitValue(encodedName.at(index + 1));
            const int low = hexDigitValue(encodedName.at(index + 2));
            if (high >= 0 && low >= 0) {
                decodedName.append(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        decodedName.append(encodedName.at(index));
    }

    return decodedName;
}

std::optional<WatermarkMarker> parseMarker(const QString &pattern)
{
    const QStringList names = pattern.trimmed().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (names.size() != 2 || !names.at(0).startsWith(u'/') || !names.at(1).startsWith(u'/') || names.at(0).size() == 1 || names.at(1).size() == 1) {
        return std::nullopt;
    }

    return WatermarkMarker{decodePdfName(names.at(0).mid(1).toUtf8()).toStdString(), decodePdfName(names.at(1).mid(1).toUtf8()).toStdString()};
}

void skipLiteralString(const QByteArray &contents, qsizetype &position)
{
    int depth = 1;
    ++position; // Opening parenthesis

    while (position < contents.size() && depth > 0) {
        const char character = contents.at(position++);
        if (character == '\\') {
            if (position < contents.size()) {
                ++position;
            }
        } else if (character == '(') {
            ++depth;
        } else if (character == ')') {
            --depth;
        }
    }
}

void skipHexString(const QByteArray &contents, qsizetype &position)
{
    ++position; // Opening angle bracket
    while (position < contents.size() && contents.at(position) != '>') {
        ++position;
    }
    if (position < contents.size()) {
        ++position;
    }
}

void skipComment(const QByteArray &contents, qsizetype &position)
{
    while (position < contents.size() && contents.at(position) != '\n' && contents.at(position) != '\r') {
        ++position;
    }
}

void skipInlineImageData(const QByteArray &contents, qsizetype &position)
{
    if (position < contents.size() && isPdfWhitespace(contents.at(position))) {
        const char firstWhitespace = contents.at(position++);
        if (firstWhitespace == '\r' && position < contents.size() && contents.at(position) == '\n') {
            ++position;
        }
    }

    while (position + 1 < contents.size()) {
        if (contents.at(position) == 'E' && contents.at(position + 1) == 'I' && position > 0 && isPdfWhitespace(contents.at(position - 1))) {
            const qsizetype end = position + 2;
            if (end == contents.size() || isPdfWhitespace(contents.at(end)) || isPdfDelimiter(contents.at(end))) {
                position = end;
                return;
            }
        }
        ++position;
    }

    position = contents.size();
}

std::optional<ContentToken> nextContentToken(const QByteArray &contents, qsizetype &position)
{
    while (position < contents.size()) {
        if (isPdfWhitespace(contents.at(position))) {
            ++position;
        } else if (contents.at(position) == '%') {
            skipComment(contents, position);
        } else {
            break;
        }
    }

    if (position >= contents.size()) {
        return std::nullopt;
    }

    ContentToken token;
    token.begin = position;
    const char firstCharacter = contents.at(position);

    if (firstCharacter == '/') {
        ++position;
        const qsizetype nameStart = position;
        while (position < contents.size() && !isPdfWhitespace(contents.at(position)) && !isPdfDelimiter(contents.at(position))) {
            ++position;
        }
        token.type = ContentTokenType::Name;
        token.value = decodePdfName(contents.sliced(nameStart, position - nameStart));
    } else if (firstCharacter == '(') {
        skipLiteralString(contents, position);
    } else if (firstCharacter == '<') {
        if (position + 1 < contents.size() && contents.at(position + 1) == '<') {
            position += 2;
        } else {
            skipHexString(contents, position);
        }
    } else if (isPdfDelimiter(firstCharacter)) {
        ++position;
    } else {
        while (position < contents.size() && !isPdfWhitespace(contents.at(position)) && !isPdfDelimiter(contents.at(position))) {
            ++position;
        }
        token.type = ContentTokenType::Keyword;
        token.value = contents.sliced(token.begin, position - token.begin);
    }

    token.end = position;
    return token;
}

RemovedWatermarkInvocations removeWatermarkInvocations(QByteArray &contents, const QSet<QByteArray> &watermarkResourceNames)
{
    RemovedWatermarkInvocations removed;
    QVector<QPair<qsizetype, qsizetype>> rangesToRemove;
    qsizetype position = 0;
    std::optional<ContentToken> previousToken;
    bool inInlineImageDictionary = false;

    while (const auto token = nextContentToken(contents, position)) {
        if (inInlineImageDictionary) {
            if (token->type == ContentTokenType::Keyword && token->value == "ID") {
                skipInlineImageData(contents, position);
                inInlineImageDictionary = false;
                previousToken.reset();
            }
            continue;
        }

        if (token->type == ContentTokenType::Keyword && token->value == "BI") {
            inInlineImageDictionary = true;
            previousToken.reset();
            continue;
        }

        if (token->type == ContentTokenType::Keyword && token->value == "Do" && previousToken && previousToken->type == ContentTokenType::Name
            && watermarkResourceNames.contains(previousToken->value)) {
            rangesToRemove.append(qMakePair(previousToken->begin, token->end));
            removed.resourceNames.insert(previousToken->value);
        }

        previousToken = token;
    }

    for (auto range = rangesToRemove.crbegin(); range != rangesToRemove.crend(); ++range) {
        contents.remove(range->first, range->second - range->first);
    }

    removed.count = static_cast<int>(rangesToRemove.size());
    return removed;
}

const PoDoFo::PdfObject *dereferenceObject(const PoDoFo::PdfObject &object, const PoDoFo::PdfMemDocument &document)
{
    PoDoFo::PdfReference reference;
    if (!object.TryGetReference(reference) || !reference.IsIndirect()) {
        return &object;
    }

    return document.GetObjects().GetObject(reference);
}

bool hasWatermarkMarker(const PoDoFo::PdfObject &object, const WatermarkMarker &marker, const PoDoFo::PdfMemDocument &document)
{
    if (!object.IsDictionary()) {
        return false;
    }

    const PoDoFo::PdfObject *privateMarker = object.GetDictionary().GetKey(marker.privateKey);
    const PoDoFo::PdfObject *resolvedMarker = privateMarker ? dereferenceObject(*privateMarker, document) : nullptr;
    return resolvedMarker != nullptr && resolvedMarker->IsName() && resolvedMarker->GetName().GetString() == marker.value;
}

bool containsWatermarkMarkerInMetadata(const PoDoFo::PdfObject &object,
                                       const WatermarkMarker &marker,
                                       const PoDoFo::PdfMemDocument &document,
                                       QSet<const PoDoFo::PdfObject *> &visitedObjects)
{
    const PoDoFo::PdfObject *resolvedObject = dereferenceObject(object, document);
    if (resolvedObject == nullptr || visitedObjects.contains(resolvedObject)) {
        return false;
    }
    visitedObjects.insert(resolvedObject);

    if (resolvedObject->IsDictionary()) {
        if (hasWatermarkMarker(*resolvedObject, marker, document)) {
            return true;
        }

        for (const auto &entry : resolvedObject->GetDictionary()) {
            if (containsWatermarkMarkerInMetadata(entry.second, marker, document, visitedObjects)) {
                return true;
            }
        }
    } else if (resolvedObject->IsArray()) {
        for (const PoDoFo::PdfObject &entry : resolvedObject->GetArray()) {
            if (containsWatermarkMarkerInMetadata(entry, marker, document, visitedObjects)) {
                return true;
            }
        }
    }

    return false;
}

bool containsWatermarkMarker(const PoDoFo::PdfObject &xObject, const WatermarkMarker &marker, const PoDoFo::PdfMemDocument &document)
{
    const PoDoFo::PdfObject *resolvedXObject = dereferenceObject(xObject, document);
    if (resolvedXObject == nullptr || !resolvedXObject->IsDictionary()) {
        return false;
    }
    if (hasWatermarkMarker(*resolvedXObject, marker, document)) {
        return true;
    }

    const PoDoFo::PdfObject *pieceInfo = resolvedXObject->GetDictionary().GetKey("PieceInfo");
    if (pieceInfo == nullptr) {
        return false;
    }

    QSet<const PoDoFo::PdfObject *> visitedObjects;
    visitedObjects.insert(resolvedXObject);
    return containsWatermarkMarkerInMetadata(*pieceInfo, marker, document, visitedObjects);
}

bool hasDigitalSignature(const PoDoFo::PdfMemDocument &document)
{
    for (const PoDoFo::PdfObject *object : document.GetObjects()) {
        if (object == nullptr || !object->IsDictionary()) {
            continue;
        }

        const PoDoFo::PdfObject *type = object->GetDictionary().GetKey("Type");
        const PoDoFo::PdfObject *resolvedType = type ? dereferenceObject(*type, document) : nullptr;
        if (resolvedType != nullptr && resolvedType->IsName() && resolvedType->GetName().GetString() == "Sig") {
            return true;
        }

        const PoDoFo::PdfObject *byteRange = object->GetDictionary().GetKey("ByteRange");
        const PoDoFo::PdfObject *contents = object->GetDictionary().GetKey("Contents");
        const PoDoFo::PdfObject *resolvedByteRange = byteRange ? dereferenceObject(*byteRange, document) : nullptr;
        if (resolvedByteRange != nullptr && resolvedByteRange->IsArray() && contents != nullptr) {
            return true;
        }
    }

    return false;
}

QSet<QByteArray> watermarkResourceNames(PoDoFo::PdfPage &page, const WatermarkMarker &marker, const PoDoFo::PdfMemDocument &document)
{
    QSet<QByteArray> names;
    for (const auto &[resourceName, xObject] : page.GetResources().GetResourceIterator(PoDoFo::PdfResourceType::XObject)) {
        if (xObject == nullptr) {
            continue;
        }

        if (!containsWatermarkMarker(*xObject, marker, document)) {
            continue;
        }

        names.insert(QByteArray(resourceName.GetString().data(), static_cast<qsizetype>(resourceName.GetString().size())));
    }

    return names;
}

int removeWatermarks(PoDoFo::PdfMemDocument &document, const WatermarkMarker &marker)
{
    int removedInvocations = 0;
    QVector<PageResourceCleanup> resourcesToCleanUp;

    for (unsigned pageIndex = 0; pageIndex < document.GetPages().GetCount(); ++pageIndex) {
        PoDoFo::PdfPage &page = document.GetPages().GetPageAt(pageIndex);
        const QSet<QByteArray> resourceNames = watermarkResourceNames(page, marker, document);
        if (resourceNames.isEmpty() || page.GetContents() == nullptr) {
            continue;
        }

        const PoDoFo::charbuff originalContents = page.MustGetContents().GetCopy();
        QByteArray filteredContents(originalContents.data(), static_cast<qsizetype>(originalContents.size()));
        const RemovedWatermarkInvocations removedOnPage = removeWatermarkInvocations(filteredContents, resourceNames);
        if (removedOnPage.count == 0) {
            continue;
        }

        PoDoFo::PdfContents &pageContents = page.GetOrCreateContents();
        pageContents.Reset();
        pageContents.CreateStreamForAppending().SetData(PoDoFo::bufferview(filteredContents.constData(), static_cast<size_t>(filteredContents.size())));
        resourcesToCleanUp.append({&page.GetResources(), removedOnPage.resourceNames});
        removedInvocations += removedOnPage.count;
    }

    for (const PageResourceCleanup &cleanup : resourcesToCleanUp) {
        for (const QByteArray &resourceName : cleanup.resourceNames) {
            static_cast<PoDoFo::PdfResourceOperations &>(*cleanup.resources).RemoveResource(PoDoFo::PdfResourceType::XObject, resourceName.toStdString());
        }
    }
    if (removedInvocations > 0) {
        document.CollectGarbage();
    }

    return removedInvocations;
}
}

PdfWatermarkFilterResult PdfWatermarkFilter::filterFile(const QString &inputFileName, const QString &outputFileName, const QString &pattern)
{
    PdfWatermarkFilterResult result;
    result.attempted = true;

    if (!QFileInfo::exists(inputFileName)) {
        result.errorString = i18n("The input PDF does not exist.");
        return result;
    }
    if (inputFileName == outputFileName) {
        result.errorString = i18n("The filtered PDF must be written to a different file.");
        return result;
    }

    const auto marker = parseMarker(pattern);
    if (!marker) {
        result.errorString = i18n("The PDF watermark marker must contain two PDF names.");
        return result;
    }

    try {
        PoDoFo::PdfMemDocument document;
        document.Load(inputFileName.toStdString());
        if (document.IsEncrypted() && !document.HasOwnerPermissions()) {
            result.errorString = i18n("The encrypted PDF cannot be modified without owner permissions.");
            return result;
        }
        if (hasDigitalSignature(document)) {
            result.errorString = i18n("The PDF contains a digital signature and cannot be modified.");
            return result;
        }
        result.occurrences = removeWatermarks(document, *marker);
        if (result.occurrences == 0) {
            return result;
        }

        document.Save(outputFileName.toStdString(), PoDoFo::PdfSaveOptions::NoMetadataUpdate);
        result.filtered = QFileInfo::exists(outputFileName) && QFileInfo(outputFileName).size() > 0;
        if (!result.filtered) {
            result.errorString = i18n("PoDoFo did not create a filtered PDF.");
        }
    } catch (const std::exception &error) {
        result.errorString = QString::fromUtf8(error.what());
    }

    return result;
}
