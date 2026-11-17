// SPDX-License-Identifier: GPL-3.0-or-later
// Transient formatting and search helpers; no persistence or EPUB mutation.
const semanticReferenceTitle = location => Array.isArray(location?.semanticPath)
    ? location.semanticPath.map(part => `${part.iast} ${part.number}`).join(", ") : ""
const compactText = value => String(value ?? "").replace(/\s+/g, " ").trim()
export const authorsForBook = book => {
    if (Array.isArray(book?.author))
        return book.author
    if (book?.author)
        return [book.author]
    return []
}

export const normalizedAuthorSet = book =>
    new Set(authorsForBook(book).map(author => compactText(author).toLocaleLowerCase()).filter(Boolean))

export const bookHasAnyAuthor = (book, authors) =>
    authorsForBook(book).some(author => authors.has(compactText(author).toLocaleLowerCase()))

export const bookTitle = book => book?.title || book?.id || ""

export const compareBooksByTitle = (left, right) =>
    bookTitle(left).localeCompare(bookTitle(right), undefined, { sensitivity: "base" })
    || (left?.id ?? "").localeCompare(right?.id ?? "", undefined, { sensitivity: "base" })

// Same hierarchy as the library: the first segment of each stored subject path.
const subjectKey = value => compactText(value).normalize("NFC").toUpperCase().toLowerCase()
const subjectsForBook = book => {
    const values = book?.subjects ?? book?.genres ?? []
    return Array.isArray(values) ? values : String(values).split(/[;,]/)
}
export const mainSubjectsForBook = book => {
    const subjects = subjectsForBook(book)
    const unique = new Map()
    for (const subject of subjects) {
        const title = compactText(String(subject).split("/")[0])
        if (title && !unique.has(subjectKey(title))) unique.set(subjectKey(title), title)
    }
    return Array.from(unique.values()).sort((a, b) => a.localeCompare(b))
}

export const defaultSourceAnchorTitle = "S.B. Vers 1.1.1"

export const plainTextForHeuristics = value =>
    compactText(`${value ?? ""}`.replace(/<[^>]+>/g, " "))

export const normalizedForHeuristics = value =>
    plainTextForHeuristics(value)
        .normalize("NFD")
        .replace(/[\u0300-\u036f]/g, "")
        .toLocaleLowerCase()

export const bookAbbreviationForTitle = book => {
    const title = bookTitle(book)
    const normalizedTitle = normalizedForHeuristics(title)
    if (/\b(srimad[-\s]*bhagavatam|srimadbhagavatam|bhagavatam)\b/.test(normalizedTitle))
        return "S.B."
    if (/\b(bhagavad[-\s]*gita|bhagavadgita|gita)\b/.test(normalizedTitle))
        return "B.G."

    const initials = plainTextForHeuristics(title)
        .replace(/['’]/g, "")
        .split(/[^A-Za-z0-9]+/)
        .filter(word => word.length > 0 && !/^(the|a|an|der|die|das|ein|eine|und|of|and)$/i.test(word))
        .slice(0, 4)
        .map(word => word[0].toLocaleUpperCase())

    return initials.length ? `${initials.join(".")}.` : "Ref."
}

export const bookAbbreviationFromReferenceText = text => {
    const raw = plainTextForHeuristics(text)
    if (/\bS\.?\s*B\.?\b/i.test(raw))
        return "S.B."
    if (/\bB\.?\s*G\.?\b/i.test(raw))
        return "B.G."

    const normalized = normalizedForHeuristics(raw)
    if (/\b(srimad[-\s]*bhagavatam|srimadbhagavatam|bhagavatam)\b/.test(normalized))
        return "S.B."
    if (/\b(bhagavad[-\s]*gita|bhagavadgita|gita)\b/.test(normalized))
        return "B.G."

    return ""
}

export const textPartsForTargetLocation = location => [
    location?.title,
    location?.tocTitle,
    Array.isArray(location?.tocPath) ? location.tocPath.join(" / ") : "",
    location?.hrefLocation,
    location?.location,
    location?.cfiLocation,
    location?.id,
].map(plainTextForHeuristics).filter(Boolean)

export const referenceCoordinateFromText = text => {
    const raw = plainTextForHeuristics(text)
    const dotted = raw.match(/(?:^|[^0-9])(\d{1,3})\.(\d{1,3})(?:\.(\d{1,3}))?(?=$|[^0-9])/)
    if (dotted)
        return [dotted[1], dotted[2], dotted[3]].filter(Boolean).join(".")

    const normalized = normalizedForHeuristics(raw)
    const delimited = normalized.match(/(?:^|[^0-9])(\d{1,3})[-_](\d{1,3})[-_](\d{1,3})(?=$|[^0-9])/)
    if (delimited)
        return `${delimited[1]}.${delimited[2]}.${delimited[3]}`

    const full = normalized.match(/(?:canto|skandha|book|buch)\D{0,20}(\d{1,3}).{0,80}(?:chapter|kapitel|adhyaya)\D{0,20}(\d{1,3}).{0,80}(?:verse|vers|text|sloka)\D{0,20}(\d{1,3})/)
    if (full)
        return `${full[1]}.${full[2]}.${full[3]}`

    return ""
}

export const referenceKindFromText = text => {
    const context = normalizedForHeuristics(text)
    if (/\b(equation|gleichung|formula|formel|eqn?|eq)\b/.test(context))
        return "Gleichung"
    if (/\b(verse|vers|sloka)\b/.test(context) || /\btext\s+\d{1,3}\b/.test(context))
        return "Vers"
    if (/\b(chapter|kapitel|canto|section|abschnitt)\b/.test(context))
        return "Kapitel"

    return ""
}

export const referenceCoordinateFromTargetLocation = location => {
    const parts = textPartsForTargetLocation(location)
    for (const part of parts) {
        const coordinate = referenceCoordinateFromText(part)
        if (coordinate)
            return coordinate
    }

    const normalizedParts = parts.map(normalizedForHeuristics)
    const canto = normalizedParts.map(part => part.match(/(?:canto|skandha|book|buch)\D{0,20}(\d{1,3})/)?.[1]).find(Boolean)
    const chapter = normalizedParts.map(part => part.match(/(?:chapter|kapitel|adhyaya)\D{0,20}(\d{1,3})/)?.[1]).find(Boolean)
    const verse = normalizedParts.map(part => part.match(/(?:verse|vers|text|sloka)\D{0,20}(\d{1,3})/)?.[1]).find(Boolean)
    if (canto && chapter && verse)
        return `${canto}.${chapter}.${verse}`
    if (chapter && verse)
        return `${chapter}.${verse}`
    if (chapter)
        return chapter

    return ""
}

export const referenceKindFromTargetLocation = (location, coordinate) => {
    const explicitKind = referenceKindFromText(textPartsForTargetLocation(location).join(" / "))
    if (explicitKind)
        return explicitKind
    if (coordinate.split(".").length >= 3)
        return "Vers"
    if (location?.type === "navigation" || location?.type === "document")
        return "Kapitel"

    return coordinate ? "Vers" : "Kapitel"
}

export const suggestedSourceAnchorTitle = (targetBook, targetLocation, sourceReferenceText = "") => {
    if (!targetBook)
        return ""

    const abbreviation = bookAbbreviationFromReferenceText(sourceReferenceText) || bookAbbreviationForTitle(targetBook)
    const semantic = semanticReferenceTitle(targetLocation)
    if (semantic) return [abbreviation, semantic].join(" ")
    const coordinate = referenceCoordinateFromText(sourceReferenceText) || referenceCoordinateFromTargetLocation(targetLocation)
    const kind = referenceKindFromText(sourceReferenceText) || referenceKindFromTargetLocation(targetLocation, coordinate)
    return [abbreviation, kind, coordinate].filter(Boolean).join(" ")
}

export const optionLabelForBook = book => {
    const authors = authorsForBook(book).join(", ")
    const title = bookTitle(book)
    return authors ? `${title} - ${authors}` : title
}

export const textPreviewForTargetLocation = location =>
    (location?.previewHtml || "").replace(/<[^>]+>/g, " ").replace(/\s+/g, " ").trim()

export const fragmentLabelForTargetLocation = location => {
    const value = location?.hrefLocation || location?.location || location?.id || ""
    const fragmentIndex = value.indexOf("#")
    if (fragmentIndex >= 0)
        return `#${value.slice(fragmentIndex + 1)}`

    const parts = value.split("/")
    return parts[parts.length - 1] || value
}

export const optionLabelForTargetLocation = location => {
    const base = location.hrefLocation || location.location || location.id || ""
    const preview = textPreviewForTargetLocation(location)
    const title = (location.title || preview || "").trim()
    const type = location.type || ""

    if (type === "navigation") {
        const label = title || base
        return label
    }

    const labelBase = fragmentLabelForTargetLocation(location)
    const labelText = title && title !== labelBase ? `${labelBase} - ${title.slice(0, 80)}` : labelBase
    return labelText
}

export const referenceSearchKey = value => String(value ?? "")
    .normalize("NFD")
    // Fold Latin diacritics (e.g. IAST); retain meaningful Devanagari vowel signs.
    .replace(/([A-Za-z])[\u0300-\u036f]+/g, "$1")
    .normalize("NFC")
    .toLocaleLowerCase()


export const groupedReferenceBooks = (books, sourceBookId, headings) => {
    const source = books.find(book => book.id === sourceBookId)
    const authors = normalizedAuthorSet(source)
    const topics = new Set(mainSubjectsForBook(source).map(subjectKey))
    return books.map(book => {
        const order = book.id === sourceBookId ? 0 : bookHasAnyAuthor(book, authors) ? 1
            : mainSubjectsForBook(book).some(subject => topics.has(subjectKey(subject))) ? 2 : 3
        const fullLabel = optionLabelForBook(book)
        const subjectText = subjectsForBook(book).join(" / ")
        return {value: book.id, label: order <= 1 ? bookTitle(book) : fullLabel,
            group: headings[order], order, data: book, tooltip: [fullLabel, subjectText].filter(Boolean).join("\n"),
            searchKey: referenceSearchKey(fullLabel + " " + book.id + " " + subjectText)}
    }).sort((a, b) => a.order - b.order || compareBooksByTitle(a.data, b.data))
}

export const matchingRows = (rows, query) => {
    const terms = referenceSearchKey(query).split(/\s+/).filter(Boolean)
    let count = 0
    const matches = []
    for (const row of rows) {
        if (!terms.every(term => row.searchKey.includes(term))) continue
        count++
        if (matches.length < 40) matches.push(row)
    }
    return {rows: matches, count}
}
