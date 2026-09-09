// SPDX-FileCopyrightText: John Factotum
// SPDX-License-Identifier: MIT

import './foliate-js/view.js'
import { FootnoteHandler } from './foliate-js/footnotes.js'
import { toPangoMarkup } from './markup.js'
import { Overlayer } from './foliate-js/overlayer.js'

let baseUrl = ""

const updateBaseUrlFromBookUrl = url => {
    const parsedUrl = new URL(url)
    baseUrl = `${parsedUrl.protocol}//${parsedUrl.host}`
}

let backend;
let openRequest = 0
const pendingActions = []
const dispatchPinchZoom = () =>
    dispatch({ type: 'pinch-zoom', payload: { scale: globalThis.visualViewport.scale } })

const yieldToBrowser = () => new Promise(resolve => setTimeout(resolve, 0))

let backendInitStarted = false
const initBackend = () => {
    if (backendInitStarted) return
    if (!globalThis.qt?.webChannelTransport || !globalThis.QWebChannel) {
        globalThis.addEventListener('load', initBackend, { once: true })
        return
    }
    backendInitStarted = true
    new QWebChannel(qt.webChannelTransport, (channel) => {
        backend = channel.objects.backend;
        while (pendingActions.length > 0) {
            backend.dispatch(pendingActions.shift())
        }
        globalThis.visualViewport.addEventListener('resize', dispatchPinchZoom)
        dispatch({ type: 'ready' })
    })
}

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initBackend, { once: true })
} else {
    initBackend()
}

const dispatch = action => {
    if (backend) {
        backend.dispatch(action)
    } else {
        pendingActions.push(action)
    }
}

const closeDialog = dialog => {
    if (dialog?.open) dialog.close()
}

let referenceDialogInitialized = false
const initReferenceDialog = () => {
    if (referenceDialogInitialized) return
    const dialog = document.getElementById('reference-dialog')
    const closeButton = document.getElementById('closeButton')
    if (dialog && closeButton) {
        closeButton.addEventListener('click', event => {
            event.preventDefault()
            closeDialog(dialog)
        })
    }

    const createDialog = document.getElementById('create-reference-dialog')
    const closeCreateButton = document.getElementById('closeCreateReferenceButton')
    if (createDialog && closeCreateButton) {
        closeCreateButton.addEventListener('click', event => {
            event.preventDefault()
            closeDialog(createDialog)
        })
    }

    referenceDialogInitialized = true
}

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initReferenceDialog, { once: true })
} else {
    initReferenceDialog()
}

let selectionAction
globalThis.showSelection = detail => new Promise(resolve => {
    selectionAction?.()
    selectionAction = resolve
    dispatch({ type: 'show-selection', payload: detail })
})

globalThis.selectionAction = action => {
    const resolve = selectionAction
    selectionAction = null
    resolve?.(action)
    globalThis.reader?.view?.deselect?.()
}

const format = {}

const debounce = (f, wait, immediate) => {
    let timeout
    return (...args) => {
        const later = () => {
            timeout = null
            if (!immediate) f(...args)
        }
        const callNow = immediate && !timeout
        if (timeout) clearTimeout(timeout)
        timeout = setTimeout(later, wait)
        if (callNow) f(...args)
    }
}

const getSelectionRange = sel => {
    if (!sel.rangeCount) return
    const range = sel.getRangeAt(0)
    if (range.collapsed) return
    return range
}

const selectionAutoTurnEnabled = view =>
    view?.renderer?.hasAttribute?.('selection-auto-turn') === true

const rangeForNode = node => {
    const range = node.ownerDocument.createRange()
    range.selectNode(node)
    return range
}

const getLang = el => {
    const lang = el.lang || el?.getAttributeNS?.('http://www.w3.org/XML/1998/namespace', 'lang')
    if (lang) return lang
    if (el.parentElement) return getLang(el.parentElement)
}

const blobToBase64 = blob => new Promise(resolve => {
    const reader = new FileReader()
    reader.readAsDataURL(blob)
    reader.onloadend = () => resolve(reader.result.split(',')[1])
})

const embedImages = async doc => {
    for (const el of doc.querySelectorAll('img[src]')) {
        const res = await fetch(el.src)
        const blob = await res.blob()
        el.src = `data:${blob.type};base64,${await blobToBase64(blob)}`
    }
}

const getHTML = async range => {
    const fragment = range.cloneContents()
    await embedImages(fragment)
    return new XMLSerializer().serializeToString(fragment)
}

const isZip = async file => {
    const arr = new Uint8Array(await file.slice(0, 4).arrayBuffer())
    return arr[0] === 0x50 && arr[1] === 0x4b && arr[2] === 0x03 && arr[3] === 0x04
}

const isPDF = async file => {
    const arr = new Uint8Array(await file.slice(0, 5).arrayBuffer())
    return arr[0] === 0x25
        && arr[1] === 0x50 && arr[2] === 0x44 && arr[3] === 0x46
        && arr[4] === 0x2d
}

const makeZipLoader = async file => {
    const { configure, ZipReader, BlobReader, TextWriter, BlobWriter } =
        await import('./foliate-js/vendor/zip.js')
    configure({ useWebWorkers: false })
    const reader = new ZipReader(new BlobReader(file))
    const entries = await reader.getEntries()
    const map = new Map(entries.map(entry => [entry.filename, entry]))
    const load = f => (name, ...args) =>
        map.has(name) ? f(map.get(name), ...args) : null
    const loadText = load(entry => entry.getData(new TextWriter()))
    const loadBlob = load((entry, type) => entry.getData(new BlobWriter(type)))
    const getSize = name => map.get(name)?.uncompressedSize ?? 0
    return { entries, loadText, loadBlob, getSize }
}

const isCBZ = ({ name, type }) =>
    type === 'application/vnd.comicbook+zip' || name.endsWith('.cbz')

const isFB2 = ({ name, type }) =>
    type === 'application/x-fictionbook+xml' || name.endsWith('.fb2')

const isFBZ = ({ name, type }) =>
    type === 'application/x-zip-compressed-fb2'
    || name.endsWith('.fb2.zip') || name.endsWith('.fbz')

const closeCurrentReader = () => {
    const reader = globalThis.reader
    if (!reader) return
    try {
        reader.close()
    } catch (e) {
        console.warn('Failed to close reader', e)
    }
    if (globalThis.reader === reader) globalThis.reader = null
}

const requestJson = (method, url, body = null) => new Promise((resolve, reject) => {
    const request = new XMLHttpRequest()
    request.open(method, url)
    if (body !== null)
        request.setRequestHeader("Content-Type", "application/json")

    request.onreadystatechange = function () {
        if (request.readyState !== 4)
            return

        if (request.status < 200 || request.status >= 300) {
            const error = new Error(request.responseText || `HTTP ${request.status}`)
            error.status = request.status
            error.responseText = request.responseText
            try {
                error.responseJson = request.responseText ? JSON.parse(request.responseText) : null
            } catch (e) {
                error.responseJson = null
            }
            reject(error)
            return
        }

        if (!request.responseText) {
            resolve(null)
            return
        }

        try {
            resolve(JSON.parse(request.responseText))
        } catch (e) {
            reject(e)
        }
    }

    request.send(body === null ? null : JSON.stringify(body))
})

const dispatchBookReloadNeeded = (reason, response) => {
    const modifiedBookIds = Array.isArray(response?.modifiedBookIds) ? response.modifiedBookIds.filter(Boolean) : []
    if (modifiedBookIds.length === 0)
        return

    dispatch({
        type: 'book-reload-needed',
        payload: {
            reason,
            modifiedBookIds,
            response: response ?? {},
        },
    })
}

const responseWithModifiedBookId = (response, bookId) => {
    if (!bookId)
        return response

    const modifiedBookIds = Array.isArray(response?.modifiedBookIds) ? response.modifiedBookIds.filter(Boolean) : []
    if (!modifiedBookIds.includes(bookId))
        modifiedBookIds.push(bookId)

    return {
        ...(response ?? {}),
        modifiedBookIds,
    }
}

const normalizedPageMode = mode => mode === 'single' || mode === 'two' ? mode : 'two'

const positiveNumberOr = (value, fallback) => {
    const number = Number(value)
    return Number.isFinite(number) && number > 0 ? number : fallback
}

const normalizedReaderLayout = layoutOrMode => {
    const layout = layoutOrMode && typeof layoutOrMode === 'object' ? layoutOrMode : {}
    const pageMode = typeof layoutOrMode === 'string'
        ? normalizedPageMode(layoutOrMode)
        : Number(layout.maxColumnCount) === 1 ? 'single' : 'two'
    const maxInlineSize = positiveNumberOr(layout.maxInlineSize, 720)
    const maxBlockSize = positiveNumberOr(layout.maxBlockSize, maxInlineSize * 2)
    const maxColumnCountValue = positiveNumberOr(layout.maxColumnCount, pageMode === 'single' ? 1 : 2)
    const maxColumnCount = Math.max(1, Math.round(maxColumnCountValue))
    const gap = positiveNumberOr(layout.gap, 0.06)

    return {
        flow: typeof layout.flow === 'string' && layout.flow.length > 0 ? layout.flow : 'paginated',
        gap,
        maxInlineSize,
        maxBlockSize,
        maxColumnCount,
        selectionAutoTurn: typeof layout.selectionAutoTurn === 'boolean'
            ? layout.selectionAutoTurn
            : maxColumnCount === 1,
        animated: layout.animated !== false,
    }
}

const applyReaderLayout = (renderer, layout) => {
    const normalizedLayout = normalizedReaderLayout(layout)
    renderer.setAttribute('flow', normalizedLayout.flow)
    renderer.setAttribute('gap', normalizedLayout.gap * 100 + '%')
    renderer.setAttribute('max-inline-size', normalizedLayout.maxInlineSize + 'px')
    renderer.setAttribute('max-block-size', normalizedLayout.maxBlockSize + 'px')
    renderer.setAttribute('max-column-count', normalizedLayout.maxColumnCount)
    if (normalizedLayout.selectionAutoTurn) renderer.setAttribute('selection-auto-turn', '')
    else renderer.removeAttribute('selection-auto-turn')
    if (normalizedLayout.animated) renderer.setAttribute('animated', '')
    else renderer.removeAttribute('animated')
    return normalizedLayout
}

const crossReferenceAnchorSelector = 'a[data-role="anchor"][data-anchor-type="crossref"][id]'
const intraBookReferenceAnchorSelector = 'a[data-role="anchor"][href][id]:not([data-anchor-type="crossref"])'
const annotationAnchorSelector = '[data-role="anchor"][data-anchor-type="annotation"][id]'
const epubAnchorSelector = '[data-role="anchor"]'
const legacyBookReferenceSelector = '.bookref[data-ref]'
const bookReferenceListSelector = `${crossReferenceAnchorSelector}, ${intraBookReferenceAnchorSelector}, ${legacyBookReferenceSelector}`
const bookReferenceClickSelector = `${crossReferenceAnchorSelector}, ${legacyBookReferenceSelector}`

const cfiFilter = node =>
    node.nodeType === Node.ELEMENT_NODE
    && (node.matches?.(epubAnchorSelector) || node.classList?.contains('bookref'))
        ? NodeFilter.FILTER_SKIP
        : NodeFilter.FILTER_ACCEPT

const referenceIdFromElement = el =>
    el.matches?.(crossReferenceAnchorSelector) || el.matches?.(intraBookReferenceAnchorSelector)
        ? el.id
        : el.dataset.ref

const compactAnnotationId = id => {
    id = `${id ?? ""}`.trim()
    return id.startsWith('uuid_') ? id.slice(5) : id
}

const annotationKey = annotation =>
    `${annotation?.annotationId || annotation?.anchorId || annotation?.cfiRange || annotation?.value || ""}`.trim()

const annotationAnchorElementIds = annotation => {
    const id = `${annotation?.anchorId || annotation?.annotationId || ""}`.trim()
    if (!id) return []

    const ids = [id]
    if (!id.startsWith('uuid_')) ids.unshift(`uuid_${id}`)
    return [...new Set(ids)]
}

const rangeForAnnotationAnchor = (doc, annotation) => {
    for (const id of annotationAnchorElementIds(annotation)) {
        const wrapper = doc.getElementById(id)
        if (wrapper?.matches?.(annotationAnchorSelector)) {
            const range = doc.createRange()
            range.selectNodeContents(wrapper)
            return range
        }

        const begin = doc.getElementById(`${id}_begin`)
        const end = doc.getElementById(`${id}_end`)
        if (begin && end) {
            const range = doc.createRange()
            range.setStartAfter(begin)
            range.setEndBefore(end)
            return range
        }
    }

    return null
}

const annotationDrawKeys = annotation => {
    const keys = [
        annotation?.value,
        annotation?.runtimeCfi,
        annotation?.cfiRange,
        annotation?.cfi,
        annotationKey(annotation),
        compactAnnotationId(annotation?.anchorId),
    ]
        .map(value => `${value ?? ""}`.trim())
        .filter(Boolean)

    return [...new Set(keys)]
}

const annotationDrawKey = annotation => annotationDrawKeys(annotation)[0] ?? ""

const referenceHrefFromElement = (section, el) => {
    const id = el.id || el.getAttribute('name') || ""
    if (section?.id && id)
        return `${section.id}#${id}`

    const href = el.getAttribute('href') ?? ""
    return href && !/^(?!blob)\w+:/i.test(href) ? href : ""
}

const rangeForReferenceElement = (doc, el) => {
    const walker = doc.createTreeWalker(el, NodeFilter.SHOW_TEXT, {
        acceptNode: node =>
            compactText(node.nodeValue).length > 0
                ? NodeFilter.FILTER_ACCEPT
                : NodeFilter.FILTER_REJECT,
    })
    let first = null
    let last = null
    for (let node = walker.nextNode(); node; node = walker.nextNode()) {
        first ??= node
        last = node
    }

    const range = doc.createRange()
    if (first && last) {
        range.setStart(first, 0)
        range.setEnd(last, last.nodeValue.length)
    } else {
        range.selectNode(el)
    }
    return range
}

const setReferenceHeader = (header, anchorTitle, fallbackTitle) => {
    if (!header)
        return

    const title = anchorTitle?.trim()
    if (!title) {
        header.textContent = fallbackTitle || "Referenz"
        return
    }

    header.replaceChildren(
        document.createTextNode("Referenz"),
        document.createElement("br"),
        document.createTextNode(title)
    )
}

const compactText = value => (value ?? "").replace(/\s+/g, " ").trim()

const authorsForBook = book => {
    if (Array.isArray(book?.author))
        return book.author
    if (book?.author)
        return [book.author]
    return []
}

const normalizedAuthorSet = book =>
    new Set(authorsForBook(book).map(author => compactText(author).toLocaleLowerCase()).filter(Boolean))

const bookHasAnyAuthor = (book, authors) =>
    authorsForBook(book).some(author => authors.has(compactText(author).toLocaleLowerCase()))

const bookTitle = book => book?.title || book?.id || ""

const compareBooksByTitle = (left, right) =>
    bookTitle(left).localeCompare(bookTitle(right), undefined, { sensitivity: "base" })
    || (left?.id ?? "").localeCompare(right?.id ?? "", undefined, { sensitivity: "base" })

const defaultSourceAnchorTitle = "S.B. Vers 1.1.1"

const plainTextForHeuristics = value =>
    compactText(`${value ?? ""}`.replace(/<[^>]+>/g, " "))

const normalizedForHeuristics = value =>
    plainTextForHeuristics(value)
        .normalize("NFD")
        .replace(/[\u0300-\u036f]/g, "")
        .toLocaleLowerCase()

const bookAbbreviationForTitle = book => {
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

const bookAbbreviationFromReferenceText = text => {
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

const textPartsForTargetLocation = location => [
    location?.title,
    location?.tocTitle,
    Array.isArray(location?.tocPath) ? location.tocPath.join(" / ") : "",
    location?.hrefLocation,
    location?.location,
    location?.cfiLocation,
    location?.id,
].map(plainTextForHeuristics).filter(Boolean)

const referenceCoordinateFromText = text => {
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

const referenceKindFromText = text => {
    const context = normalizedForHeuristics(text)
    if (/\b(equation|gleichung|formula|formel|eqn?|eq)\b/.test(context))
        return "Gleichung"
    if (/\b(verse|vers|sloka)\b/.test(context) || /\btext\s+\d{1,3}\b/.test(context))
        return "Vers"
    if (/\b(chapter|kapitel|canto|section|abschnitt)\b/.test(context))
        return "Kapitel"

    return ""
}

const referenceCoordinateFromTargetLocation = location => {
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

const referenceKindFromTargetLocation = (location, coordinate) => {
    const explicitKind = referenceKindFromText(textPartsForTargetLocation(location).join(" / "))
    if (explicitKind)
        return explicitKind
    if (coordinate.split(".").length >= 3)
        return "Vers"
    if (location?.type === "navigation" || location?.type === "document")
        return "Kapitel"

    return coordinate ? "Vers" : "Kapitel"
}

const suggestedSourceAnchorTitle = (targetBook, targetLocation, sourceReferenceText = "") => {
    if (!targetBook)
        return ""

    const abbreviation = bookAbbreviationFromReferenceText(sourceReferenceText) || bookAbbreviationForTitle(targetBook)
    const coordinate = referenceCoordinateFromText(sourceReferenceText) || referenceCoordinateFromTargetLocation(targetLocation)
    const kind = referenceKindFromText(sourceReferenceText) || referenceKindFromTargetLocation(targetLocation, coordinate)
    return [abbreviation, kind, coordinate].filter(Boolean).join(" ")
}

const optionLabelForBook = book => {
    const authors = authorsForBook(book).join(", ")
    const title = bookTitle(book)
    return authors ? `${title} - ${authors}` : title
}

const appendTargetBookGroup = (select, label, books, selectedBookId) => {
    if (!books.length)
        return

    const group = document.createElement("optgroup")
    group.label = label

    for (const book of books) {
        const option = document.createElement("option")
        option.value = book.id
        option.textContent = optionLabelForBook(book)
        option.selected = book.id === selectedBookId
        option.dataset.bookTitle = bookTitle(book)
        option.dataset.bookAuthors = authorsForBook(book).join(", ")
        group.append(option)
    }

    select.append(group)
}

const populateTargetBookSelect = async (targetBookSelect, sourceBookId) => {
    const data = await requestJson("GET", `${baseUrl}/books`)
    const booksById = new Map((Array.isArray(data) ? data : data?.books ?? [])
        .filter(book => book?.id)
        .map(book => [book.id, book]))

    if (sourceBookId && !booksById.has(sourceBookId))
        booksById.set(sourceBookId, { id: sourceBookId, title: sourceBookId, author: [] })

    const books = Array.from(booksById.values()).sort(compareBooksByTitle)
    const ownBook = sourceBookId ? booksById.get(sourceBookId) : null
    const ownAuthors = normalizedAuthorSet(ownBook)
    const sameAuthorBooks = ownAuthors.size
        ? books.filter(book => book.id !== sourceBookId && bookHasAnyAuthor(book, ownAuthors)).sort(compareBooksByTitle)
        : []
    const sameAuthorBookIds = new Set(sameAuthorBooks.map(book => book.id))
    const otherBooks = books
        .filter(book => book.id !== sourceBookId && !sameAuthorBookIds.has(book.id))
        .sort(compareBooksByTitle)

    targetBookSelect.textContent = ""

    const placeholder = document.createElement("option")
    placeholder.value = ""
    placeholder.textContent = "-- Bitte wählen --"
    placeholder.selected = !ownBook
    targetBookSelect.append(placeholder)

    appendTargetBookGroup(targetBookSelect, "Dieses Buch", ownBook ? [ownBook] : [], sourceBookId)
    appendTargetBookGroup(targetBookSelect, "Bücher vom selben Autor", sameAuthorBooks, sourceBookId)
    appendTargetBookGroup(targetBookSelect, "Andere Bücher", otherBooks, sourceBookId)
}

const textPreviewForTargetLocation = location =>
    (location?.previewHtml || "").replace(/<[^>]+>/g, " ").replace(/\s+/g, " ").trim()

const fragmentLabelForTargetLocation = location => {
    const value = location?.hrefLocation || location?.location || location?.id || ""
    const fragmentIndex = value.indexOf("#")
    if (fragmentIndex >= 0)
        return `#${value.slice(fragmentIndex + 1)}`

    const parts = value.split("/")
    return parts.at(-1) || value
}

const optionLabelForTargetLocation = location => {
    const base = location.hrefLocation || location.location || location.id || ""
    const preview = textPreviewForTargetLocation(location)
    const title = (location.title || preview || "").trim()
    const numericDepth = Number(location.tocDepth)
    const depth = Number.isFinite(numericDepth) ? Math.max(0, Math.min(numericDepth, 8)) : 0
    const type = location.type || ""

    if (type === "navigation") {
        const label = title || base
        return `${"  ".repeat(depth)}${label}`
    }

    const labelBase = fragmentLabelForTargetLocation(location)
    const labelText = title && title !== labelBase ? `${labelBase} - ${title.slice(0, 80)}` : labelBase
    const childDepth = location.tocTitle ? Math.max(depth, 1) : depth
    return `${"  ".repeat(childDepth)}- ${labelText}`
}

const setTargetLocationFields = (option, targetLocationInput, targetPreviewHtmlInput, targetCFIInput, options = {}) => {
    const updatePreviewHtml = options.updatePreviewHtml !== false
    const hasSelection = Boolean(option?.value)
    if (targetLocationInput)
        targetLocationInput.value = hasSelection ? option.value : ""
    if (targetPreviewHtmlInput && updatePreviewHtml) {
        targetPreviewHtmlInput.value = hasSelection ? option.dataset.previewHtml ?? "" : ""
        targetPreviewHtmlInput.dataset.previewEdited = "false"
    } else if (targetPreviewHtmlInput && !hasSelection && targetPreviewHtmlInput.dataset.previewEdited !== "true") {
        targetPreviewHtmlInput.value = ""
    }
    if (targetCFIInput) {
        if (hasSelection)
            targetCFIInput.value = ""
        targetCFIInput.disabled = hasSelection
    }
}

const populateTargetLocationSelect = async (targetBookId, targetLocationSelect, targetLocationInput, targetPreviewHtmlInput, targetCFIInput) => {
    if (!targetLocationSelect)
        return

    targetLocationSelect.textContent = ""
    setTargetLocationFields(null, targetLocationInput, targetPreviewHtmlInput, targetCFIInput)

    const placeholder = document.createElement("option")
    placeholder.value = ""
    placeholder.textContent = "-- Keine Auswahl --"
    targetLocationSelect.append(placeholder)

    if (!targetBookId)
        return

    const data = await requestJson("GET", `${baseUrl}/${encodeURIComponent(targetBookId)}/targetLocations`)
    const locations = Array.isArray(data) ? data : data?.targetLocations ?? []

    for (const location of locations) {
        if (!location?.id || !location?.location)
            continue

        const readerLocation = location.readerLocation || location.location
        const hrefLocation = location.hrefLocation || location.location
        const option = document.createElement("option")
        option.value = readerLocation
        option.textContent = optionLabelForTargetLocation(location)
        option.dataset.location = readerLocation
        option.dataset.hrefLocation = hrefLocation
        option.dataset.cfiLocation = location.cfiLocation || ""
        option.dataset.previewHtml = location.previewHtml ?? ""
        option.dataset.locationTitle = location.title ?? ""
        option.dataset.locationType = location.type ?? ""
        option.dataset.tocTitle = location.tocTitle ?? ""
        option.dataset.tocPath = JSON.stringify(Array.isArray(location.tocPath) ? location.tocPath : [])
        option.title = [Array.isArray(location.tocPath) ? location.tocPath.join(" / ") : "", hrefLocation, location.cfiLocation || "", textPreviewForTargetLocation(location)]
            .filter(Boolean)
            .join("\n")
        targetLocationSelect.append(option)
    }
}

const selectedTargetBookForTitle = targetBookSelect => {
    const option = targetBookSelect?.selectedOptions?.[0]
    if (!option?.value)
        return null

    return {
        id: option.value,
        title: option.dataset.bookTitle || option.textContent || option.value,
        author: option.dataset.bookAuthors || "",
    }
}

const selectedTargetLocationForTitle = targetLocationSelect => {
    const option = targetLocationSelect?.selectedOptions?.[0]
    if (!option?.value)
        return null

    let tocPath = []
    try {
        tocPath = JSON.parse(option.dataset.tocPath || "[]")
    } catch {
        tocPath = []
    }

    return {
        id: option.value,
        location: option.dataset.location || option.value,
        hrefLocation: option.dataset.hrefLocation || "",
        cfiLocation: option.dataset.cfiLocation || "",
        previewHtml: option.dataset.previewHtml || "",
        title: option.dataset.locationTitle || "",
        type: option.dataset.locationType || "",
        tocTitle: option.dataset.tocTitle || "",
        tocPath: Array.isArray(tocPath) ? tocPath : [],
    }
}

const updateSourceAnchorTitleSuggestion = (sourceAnchorTitleInput, targetBookSelect, targetLocationSelect, sourceReferenceText = "", force = false) => {
    if (!sourceAnchorTitleInput)
        return

    if (!force && sourceAnchorTitleInput.dataset.autoTitle === "false" && sourceAnchorTitleInput.value.trim())
        return

    const suggestion = suggestedSourceAnchorTitle(
        selectedTargetBookForTitle(targetBookSelect),
        selectedTargetLocationForTitle(targetLocationSelect),
        sourceReferenceText
    ) || defaultSourceAnchorTitle
    sourceAnchorTitleInput.value = suggestion
    sourceAnchorTitleInput.dataset.autoTitle = "true"
}

const refreshReferenceOverview = () => {
    try {
        if (backend && typeof backend.loadReferenceOverview === "function")
            backend.loadReferenceOverview()
    } catch (error) {
        console.warn("Unable to refresh reference overview", error)
    }
}

const selectTargetLocationOption = (targetLocationSelect, targetLocationInput, targetPreviewHtmlInput, targetCFIInput, preferred = {}) => {
    if (!targetLocationSelect)
        return false

    const preferredLocation = preferred.targetLocation ?? ""
    const preferredLocations = [preferredLocation, preferred.targetHrefLocation ?? "", preferred.targetCfiLocation ?? ""].filter(Boolean)
    let optionToSelect = null
    for (const option of Array.from(targetLocationSelect.options)) {
        if (!option.value)
            continue

        if (preferredLocations.some(location =>
            option.value === location || option.dataset.location === location || option.dataset.hrefLocation === location || option.dataset.cfiLocation === location)) {
            optionToSelect = option
            break
        }
    }

    if (!optionToSelect && preferredLocation) {
        optionToSelect = document.createElement("option")
        optionToSelect.value = preferredLocation
        optionToSelect.textContent = preferredLocation
        optionToSelect.dataset.location = preferredLocation
        optionToSelect.dataset.hrefLocation = preferred.targetHrefLocation ?? ""
        optionToSelect.dataset.cfiLocation = preferred.targetCfiLocation ?? (preferredLocation.startsWith("epubcfi(") ? preferredLocation : "")
        optionToSelect.dataset.previewHtml = preferred.targetPreviewHtml ?? ""
        optionToSelect.dataset.locationTitle = preferred.targetLocationTitle ?? ""
        optionToSelect.dataset.locationType = preferred.targetLocationType ?? ""
        optionToSelect.dataset.tocTitle = preferred.targetTocTitle ?? ""
        optionToSelect.dataset.tocPath = JSON.stringify(Array.isArray(preferred.targetTocPath) ? preferred.targetTocPath : [])
        optionToSelect.title = [preferredLocation, textPreviewForTargetLocation({ previewHtml: preferred.targetPreviewHtml ?? "" })]
            .filter(Boolean)
            .join("\n")
        targetLocationSelect.append(optionToSelect)
    }

    if (!optionToSelect)
        return false

    if (Object.prototype.hasOwnProperty.call(preferred, "targetPreviewHtml")) {
        optionToSelect.dataset.previewHtml = preferred.targetPreviewHtml ?? ""
    }

    targetLocationSelect.value = optionToSelect.value
    setTargetLocationFields(optionToSelect, targetLocationInput, targetPreviewHtmlInput, targetCFIInput)
    return true
}

const confirmReferenceDeletion = () => new Promise(resolve => {
    const dialog = document.getElementById("delete-reference-dialog")
    if (!dialog) {
        resolve(globalThis.confirm?.("Referenz wirklich löschen?") ?? false)
        return
    }

    const cancelButton = document.getElementById("cancelDeleteReferenceButton")
    const confirmButton = document.getElementById("confirmDeleteReferenceButton")
    let settled = false
    const finish = result => {
        if (settled)
            return

        settled = true
        if (cancelButton)
            cancelButton.onclick = null
        if (confirmButton)
            confirmButton.onclick = null
        dialog.oncancel = null
        dialog.onclose = null
        closeDialog(dialog)
        resolve(result)
    }

    if (cancelButton)
        cancelButton.onclick = event => {
            event.preventDefault()
            finish(false)
        }
    if (confirmButton)
        confirmButton.onclick = event => {
            event.preventDefault()
            finish(true)
        }
    dialog.oncancel = event => {
        event.preventDefault()
        finish(false)
    }
    dialog.onclose = () => finish(false)

    if (!dialog.open)
        dialog.showModal()
})

const hasReferencePreviewHtml = previewHtml => {
    if (previewHtml === null || previewHtml === undefined)
        return false

    return String(previewHtml).trim().length > 0
}

const createReference = (sourceBookId, cfi, text, callback, options = {}) => {
    const createReferenceDialog = document.getElementById("create-reference-dialog")
    const createReferenceHeader = document.getElementById("createReferenceHeader")
    const createReferenceForm = document.getElementById("createReferenceForm")
    const targetBookSelect = document.getElementById("targetBookId")
    const targetLocationSelect = document.getElementById("targetLocationSelect")
    const sourceAnchorTitleInput = document.getElementById("sourceAnchorTitle")
    const targetCFIInput = document.getElementById("targetCFI")
    const targetLocationInput = document.getElementById("targetLocation")
    const targetPreviewHtmlInput = document.getElementById("targetPreviewHtml")
    const submitReferenceButton = document.getElementById("submitReferenceButton")
    const editMode = Boolean(options.editMode)

    if (!createReferenceDialog || !createReferenceForm || !targetBookSelect || !targetLocationSelect) {
        console.warn("Create reference dialog is missing required elements")
        callback(false)
        return
    }

    createReferenceForm.reset()
    if (createReferenceHeader)
        createReferenceHeader.textContent = editMode ? "Referenz bearbeiten" : "Referenz erstellen"
    if (submitReferenceButton)
        submitReferenceButton.textContent = editMode ? (globalThis.uiText?.referenceDialog?.saveChangeOfReference ?? "save change of reference") : "Referenz erstellen"
    if (sourceAnchorTitleInput) {
        sourceAnchorTitleInput.value = options.sourceAnchorTitle ?? ""
        sourceAnchorTitleInput.dataset.autoTitle = options.sourceAnchorTitle ? "false" : "true"
        sourceAnchorTitleInput.oninput = () => {
            sourceAnchorTitleInput.dataset.autoTitle = "false"
        }
    }
    if (targetPreviewHtmlInput) {
        targetPreviewHtmlInput.value = ""
        targetPreviewHtmlInput.dataset.previewEdited = "false"
        targetPreviewHtmlInput.oninput = () => {
            targetPreviewHtmlInput.dataset.previewEdited = "true"
        }
    }
    populateTargetLocationSelect("", targetLocationSelect, targetLocationInput, targetPreviewHtmlInput, targetCFIInput)
    populateTargetBookSelect(targetBookSelect, sourceBookId)
        .then(async () => {
            const preferredTarget = {
                targetLocation: options.targetLocation ?? "",
                targetHrefLocation: options.targetHrefLocation ?? "",
                targetCfiLocation: options.targetCfiLocation ?? "",
                targetPreviewHtml: options.targetPreviewHtml ?? "",
            }
            if (options.targetBookId)
                targetBookSelect.value = options.targetBookId

            const refreshTargetLocations = (preferred = {}) => {
                if (targetCFIInput)
                    targetCFIInput.value = ""
                return populateTargetLocationSelect(targetBookSelect.value, targetLocationSelect, targetLocationInput, targetPreviewHtmlInput, targetCFIInput)
                    .then(() => {
                        selectTargetLocationOption(targetLocationSelect, targetLocationInput, targetPreviewHtmlInput, targetCFIInput, preferred)
                        updateSourceAnchorTitleSuggestion(sourceAnchorTitleInput, targetBookSelect, targetLocationSelect, text)
                    })
                    .catch(error => console.warn("Unable to load target locations", error))
            }
            targetBookSelect.oninput = () => refreshTargetLocations()
            targetBookSelect.onchange = () => refreshTargetLocations()
            targetLocationSelect.onchange = () => {
                setTargetLocationFields(targetLocationSelect.selectedOptions[0], targetLocationInput, targetPreviewHtmlInput, targetCFIInput)
                updateSourceAnchorTitleSuggestion(sourceAnchorTitleInput, targetBookSelect, targetLocationSelect, text)
            }
            if (targetCFIInput) {
                targetCFIInput.oninput = () => {
                    if (targetCFIInput.value.trim()) {
                        targetLocationSelect.value = ""
                        setTargetLocationFields(null, targetLocationInput, targetPreviewHtmlInput, targetCFIInput, { updatePreviewHtml: false })
                        updateSourceAnchorTitleSuggestion(sourceAnchorTitleInput, targetBookSelect, targetLocationSelect, text)
                    }
                }
            }

            updateSourceAnchorTitleSuggestion(sourceAnchorTitleInput, targetBookSelect, targetLocationSelect, text)

            if (targetBookSelect.value)
                await refreshTargetLocations(preferredTarget)

            createReferenceForm.onsubmit = event => {
                event.preventDefault()
                const hasSelectedTargetLocation = Boolean(targetLocationSelect.value)
                if (hasSelectedTargetLocation)
                    setTargetLocationFields(targetLocationSelect.selectedOptions[0], targetLocationInput, targetPreviewHtmlInput, targetCFIInput, { updatePreviewHtml: false })

                const targetLocation = hasSelectedTargetLocation ? targetLocationInput?.value ?? "" : ""
                const targetPreviewHtml = targetPreviewHtmlInput?.value ?? ""
                const targetPreviewHtmlEdited = targetPreviewHtmlInput?.dataset.previewEdited === "true"
                const targetCFI = hasSelectedTargetLocation ? "" : (targetCFIInput?.value.trim() ?? "")

                if (!targetBookSelect.value || (!hasSelectedTargetLocation && !targetCFI)) {
                    console.warn("Reference target is missing")
                    callback(false)
                    return
                }

                requestJson("POST", `${baseUrl}/${encodeURIComponent(sourceBookId)}/ref`, {
                    cfi,
                    sourceAnchorId: options.sourceAnchorId ?? "",
                    sourceAnchorTitle: sourceAnchorTitleInput?.value.trim() ?? "",
                    text,
                    targetBookId: targetBookSelect.value,
                    targetLocation,
                    targetPreviewHtml,
                    targetPreviewHtmlEdited,
                    targetCFI,
                })
                    .then(response => {
                        const reloadResponse = responseWithModifiedBookId(response, sourceBookId)
                        closeDialog(createReferenceDialog)
                        dispatchBookReloadNeeded(editMode ? "reference-anchor-updated" : "reference-anchor-created", reloadResponse)
                        callback(true, reloadResponse)
                    })
                    .catch(error => {
                        if (error.status === 409 && error.responseJson?.error === "manualAnchorRequired") {
                            dispatch({
                                type: 'manual-anchor-required',
                                payload: {
                                    ...error.responseJson,
                                    cfi: error.responseJson.cfi || cfi,
                                    text: error.responseJson.text || text
                                }
                            })
                            if (error.responseJson.referenceStored)
                                closeDialog(createReferenceDialog)
                            const reloadResponse = error.responseJson.referenceStored
                                ? responseWithModifiedBookId(error.responseJson, sourceBookId)
                                : error.responseJson
                            dispatchBookReloadNeeded("reference-anchor-created", reloadResponse)
                            callback(Boolean(error.responseJson.referenceStored), reloadResponse)
                            return
                        }

                        console.warn("Unable to create reference", error)
                        callback(false)
                    })
            }

            if (!createReferenceDialog.open)
                createReferenceDialog.showModal()
        })
        .catch(error => {
            console.warn("Unable to load book list", error)
            callback(false)
        })
}

const setImageNotInverse = (bookId, imageContext) => {
    if (!bookId || !imageContext?.cfi)
        return Promise.reject(new Error("Missing image not_inverse context"))

    return requestJson("POST", `${baseUrl}/${encodeURIComponent(bookId)}/image/not-inverse`, {
        cfi: imageContext.cfi,
        src: imageContext.src ?? "",
    })
}

const open = async (url, initCfi, bookId, initialLayout) => {
    const request = ++openRequest
    closeCurrentReader()
    updateBaseUrlFromBookUrl(url)

    const response = await fetch(url);
    if (!response.ok) {
        const responseText = await response.text()
        let payload = {
            httpStatus: response.status,
            status: `http-${response.status}`,
            message: responseText || `HTTP ${response.status}`,
        }
        try {
            const responseJson = responseText ? JSON.parse(responseText) : null
            if (responseJson && typeof responseJson === 'object') {
                payload = { ...responseJson, httpStatus: response.status }
            }
        } catch (e) {
            // Keep the textual payload for non-JSON HTTP errors.
        }
        dispatch({ type: 'book-error', payload })
        return
    }

    const file = await response.blob();
    file.name = response.url.split('/').pop();
    await yieldToBrowser()

    if (request !== openRequest) return

    if (!file.size) {
        dispatch({ type: 'book-error', payload: 'not-found' })
        return
    }

    let book
    if (await isZip(file)) {
        const loader = await makeZipLoader(file)
        await yieldToBrowser()
        const { entries } = loader
        if (isCBZ(file)) {
            const { makeComicBook } = await import('./foliate-js/comic-book.js')
            book = makeComicBook(loader, file)
        } else if (isFBZ(file)) {
            const { makeFB2 } = await import('./foliate-js/fb2.js')
            const entry = entries.find(entry => entry.filename.endsWith('.fb2'))
            const blob = await loader.loadBlob((entry ?? entries[0]).filename)
            book = await makeFB2(blob)
        } else {
            const { EPUB } = await import('./foliate-js/epub.js')
            book = await new EPUB(loader).init()
            await yieldToBrowser()
        }
    }
    else if (await isPDF(file)) {
        const { makePDF } = await import('./foliate-js/pdf.js')
        book = await makePDF(file)
    }
    else {
        const { isMOBI, MOBI } = await import('./foliate-js/mobi.js')
        if (await isMOBI(file)) {
            const fflate = await import('./foliate-js/vendor/fflate.js')
            book = await new MOBI({ unzlib: fflate.unzlibSync }).open(file)
        } else if (isFB2(file)) {
            const { makeFB2 } = await import('./foliate-js/fb2.js')
            book = await makeFB2(file)
        }
    }

    if (request !== openRequest) {
        book?.destroy?.()
        return
    }

    if (!book) {
        dispatch({ type: 'book-error', payload: 'unsupported-type' }) //payload type will change here.
        return
    }
    const reader = new Reader(book, initCfi, bookId, initialLayout)
    globalThis.reader = reader
    await reader.init()
    if (request !== openRequest) {
        if (globalThis.reader === reader) globalThis.reader = null
        reader.close()
        return
    }
    const readyBook = {
        metadata: book.metadata ?? null,
        toc: book.toc ?? null,
        pageList: book.pageList ?? null,
    }
    dispatch({ type: 'book-ready', payload: { book: readyBook } })
}

globalThis.openSync = function (url, initCfi, bookId, initialLayout) {
    open(url, initCfi, bookId, initialLayout).catch(e => {
        console.error(e)
        dispatch({ type: 'book-error', payload: e?.message ?? String(e) })
    });
}

globalThis.closeReader = () => {
    openRequest += 1
    closeCurrentReader()
}

const getCSS = ({
    lineHeight, justify, hyphenate, invert, theme, overrideFont, userStylesheet, fontSize,
    mediaActiveClass,
}) => [`
    @namespace epub "http://www.idpf.org/2007/ops";
    @media print {
        html {
            column-width: auto !important;
            height: auto !important;
            width: auto !important;
        }
    }
    @media screen {
        html {
            color-scheme: ${invert ? 'only light' : 'light dark'};
            color: ${theme.light.fg};
        }
        a:any-link {
            color: ${theme.light.link};
        }
        @media (prefers-color-scheme: dark) {
            html {
                color: ${invert ? theme.inverted.fg : theme.dark.fg};
                ${invert ? '-webkit-font-smoothing: antialiased;' : ''}
            }
            a:any-link {
                color: ${invert ? theme.inverted.link : theme.dark.link};
            }
        }
        aside[epub|type~="footnote"] {
            display: none;
        }
    }
    html {
        line-height: ${lineHeight};
        font-size: ${fontSize}pt;
        hanging-punctuation: allow-end last;
        orphans: 2;
        widows: 2;
    }
    [align="left"] { text-align: left; }
    [align="right"] { text-align: right; }
    [align="center"] { text-align: center; }
    [align="justify"] { text-align: justify; }
    :is(hgroup, header) p {
        text-align: unset;
        hyphens: unset;
    }
    pre {
        white-space: pre-wrap !important;
        tab-size: 2;
    }
`, `
    a[href]:is(:link, :visited, :hover, :active, :focus):not([href*=":"]):not([href^="//"]) {
        text-decoration: none !important;
    }
    ${invert ? `
    @media screen {
        html, body {
            color: ${theme.inverted.fg} !important;
            background: ${theme.inverted.bg} !important;
        }
        body * {
            color: inherit !important;
            border-color: currentColor !important;
            background-color: ${theme.inverted.bg} !important;
        }
        a:any-link {
            color: ${theme.inverted.link} !important;
        }
        svg, img {
            background-color: transparent !important;
        }
        .${CSS.escape(mediaActiveClass)}, .${CSS.escape(mediaActiveClass)} * {
            color: ${theme.inverted.fg} !important;
            background: color-mix(in hsl, ${theme.inverted.fg}, ${theme.inverted.bg} 85%) !important;
        }
    }` : ''}
    @media screen and (prefers-color-scheme: light) {
        ${!invert && theme.light.bg !== '#ffffff' ? `
        html, body {
            color: ${theme.light.fg} !important;
            background: ${theme.light.bg} !important;
        }
        body * {
            color: inherit !important;
            border-color: currentColor !important;
            background-color: ${theme.light.bg} !important;
        }
        a:any-link {
            color: ${theme.light.link} !important;
        }
        svg, img {
            background-color: transparent !important;
            mix-blend-mode: multiply;
        }
        .${CSS.escape(mediaActiveClass)}, .${CSS.escape(mediaActiveClass)} * {
            color: ${theme.light.fg} !important;
            background: color-mix(in hsl, ${theme.light.fg}, #fff 50%) !important;
            background: color-mix(in hsl, ${theme.light.fg}, ${theme.light.bg} 85%) !important;
        }` : ''}
    }
    @media screen and (prefers-color-scheme: dark) {
        ${invert ? '' : `
        html, body {
            color: ${theme.dark.fg} !important;
            background: ${theme.dark.bg} !important;
        }
        body * {
            color: inherit !important;
            border-color: currentColor !important;
            background-color: ${theme.dark.bg} !important;
        }
        a:any-link {
            color: ${theme.dark.link} !important;
        }
        .${CSS.escape(mediaActiveClass)}, .${CSS.escape(mediaActiveClass)} * {
            color: ${theme.dark.fg} !important;
            background: color-mix(in hsl, ${theme.dark.fg}, #000 50%) !important;
            background: color-mix(in hsl, ${theme.dark.fg}, ${theme.dark.bg} 75%) !important;
        }`}
    }
    p, li, blockquote, dd {
        line-height: ${lineHeight};
        text-align: ${justify ? 'justify' : 'start'};
        hyphens: ${hyphenate ? 'auto' : 'none'};
    }
    ${overrideFont ? '* { font-family: revert !important }' : ''}
` + userStylesheet]

const pointIsInView = ({ x, y }) =>
    x > 0 && y > 0 && x < window.innerWidth && y < window.innerHeight

const getPosition = target => {
    // TODO: vertical text
    const frameElement = (target.getRootNode?.() ?? target?.endContainer?.getRootNode?.())
        ?.defaultView?.frameElement

    const transform = frameElement ? getComputedStyle(frameElement).transform : ''
    const match = transform.match(/matrix\((.+)\)/)
    const [sx, , , sy] = match?.[1]?.split(/\s*,\s*/)?.map(x => parseFloat(x)) ?? []

    const frame = frameElement?.getBoundingClientRect() ?? { top: 0, left: 0 }
    const rects = Array.from(target.getClientRects())
    const first = frameRect(frame, rects[0], sx, sy)
    const last = frameRect(frame, rects.at(-1), sx, sy)
    const start = {
        point: { x: (first.left + first.right) / 2, y: first.top },
        dir: 'up',
    }
    const end = {
        point: { x: (last.left + last.right) / 2, y: last.bottom },
        dir: 'down',
    }
    const startInView = pointIsInView(start.point)
    const endInView = pointIsInView(end.point)
    if (!startInView && !endInView) return { point: { x: 0, y: 0 } }
    if (!startInView) return end
    if (!endInView) return start
    return start.point.y > window.innerHeight - end.point.y ? start : end
}

const footnoteDialog = document.getElementById('footnote-dialog')
footnoteDialog.addEventListener('close', () => {
    dispatch({ type: 'dialog-close' })
    const view = footnoteDialog.querySelector('foliate-view')
    view.close()
    view.remove()
    if (footnoteDialog.returnValue === 'go')
        globalThis.reader.view.goTo(footnoteDialog.querySelector('[name="href"]').value)
    footnoteDialog.returnValue = null
})
footnoteDialog.addEventListener('click', e =>
    e.target === footnoteDialog ? footnoteDialog.close() : null)

// getRect function in epub implementation
const frameRect = (frame, rect, sx = 1, sy = 1) => {
    const left = sx * rect.left + frame.left
    const right = sx * rect.right + frame.left
    const top = sy * rect.top + frame.top
    const bottom = sy * rect.bottom + frame.top
    return { left, right, top, bottom }
}

class CursorAutohider {
    #timeout
    #el
    #check
    #state
    constructor(el, check, state = {}) {
        this.#el = el
        this.#check = check
        this.#state = state
        if (this.#state.hidden) this.hide()
        this.#el.addEventListener('mousemove', ({ screenX, screenY }) => {
            // check if it actually moved
            if (screenX === this.#state.x && screenY === this.#state.y) return
            this.#state.x = screenX, this.#state.y = screenY
            this.show()
            if (this.#timeout) clearTimeout(this.#timeout)
            if (check()) this.#timeout = setTimeout(this.hide.bind(this), 1000)
        }, false)
    }
    cloneFor(el) {
        return new CursorAutohider(el, this.#check, this.#state)
    }
    hide() {
        this.#el.style.cursor = 'none'
        this.#state.hidden = true
    }
    show() {
        this.#el.style.cursor = 'auto'
        this.#state.hidden = false
    }
}

// Create the Reader class : init->'foliate-view', handleEvents()

class BookReferenceHandler {
    constructor(bookId) {
        this.bookId = bookId;
    }

    async #loadReference(book, detail = {}, options = {}) {
        const { ref, text = "", title = "" } = detail;
        if (!ref)
            return null

        const source = ref;

        const ReferenceResponse = await fetch(
            `${baseUrl}/${encodeURIComponent(this.bookId)}/ref/${encodeURIComponent(ref)}`
        );
        if (ReferenceResponse.status === 404) {
            if (options.createIfMissing === false)
                return null

            createReference(this.bookId, "", text, success => {
                if (success) console.log("Reference stored for existing source anchor:", ref);
                else console.log("Reference not stored for existing source anchor:", ref);
            }, {
                sourceAnchorId: ref,
                sourceAnchorTitle: title,
            })
            return null
        }
        if (!ReferenceResponse.ok) {
            throw new Error(`Unable to load reference ${ref}: HTTP ${ReferenceResponse.status}`)
        }
        const reference = await ReferenceResponse.json();
        const sourceAnchorTitle = reference.sourceAnchorTitle ?? title
        const targetLocation = reference.location ?? reference.targetLocation ?? reference.target?.location ?? ""
        const targetPreviewHtml = reference.target?.previewHtml ?? reference.targetPreviewHtml ?? reference.previewHtml ?? ""
        const targetBookId = reference.targetBookId ?? reference.target?.targetBookId ?? ""
        const targetHrefLocation = reference.targetHrefLocation ?? reference.target?.hrefLocation ?? ""
        const targetCfiLocation = reference.targetCfiLocation ?? reference.target?.cfiLocation ?? ""
        const targetNavigationLocation = targetHrefLocation || targetLocation
        const targetEntry = reference.target?.entry ?? null
        const sourceTitle = book?.metadata?.title || book?.metadata?.identifier || ''

        return {
            ref,
            text,
            title,
            source,
            reference,
            sourceAnchorTitle,
            targetLocation,
            targetPreviewHtml,
            targetBookId,
            targetHrefLocation,
            targetCfiLocation,
            targetNavigationLocation,
            targetEntry,
            sourceTitle,
        }
    }

    #openReference(context, dialog = null) {
        if (!context?.targetNavigationLocation) {
            console.warn('No target location available for reference');
            return false;
        }

        try {
            if (backend && typeof backend.openReferencePage === 'function') {
                closeDialog(dialog);
                backend.openReferencePage(
                    context.targetNavigationLocation,
                    context.targetEntry,
                    context.reference.target?.readOnly ?? false,
                    context.sourceTitle
                );
                return true;
            }

            console.warn('backend.openReferencePage is not available');
        } catch (e) {
            console.warn('Failed to open reference page', e);
        }

        return false;
    }

    #editReference(book, context, dialog = null) {
        closeDialog(dialog)
        createReference(this.bookId, "", context.text, (success, response = {}) => {
            if (!success)
                return

            const updatedSourceAnchorTitle = response.sourceAnchorTitle ?? context.sourceAnchorTitle
            refreshReferenceOverview()
            this.handle(book, {
                detail: {
                    ref: context.ref,
                    text: context.text,
                    title: updatedSourceAnchorTitle,
                }
            }).catch(error => console.warn("Unable to reload edited reference", error))
        }, {
            editMode: true,
            sourceAnchorId: context.ref,
            sourceAnchorTitle: context.sourceAnchorTitle,
            targetBookId: context.targetBookId,
            targetLocation: context.targetLocation,
            targetHrefLocation: context.targetHrefLocation,
            targetCfiLocation: context.targetCfiLocation,
            targetPreviewHtml: context.targetPreviewHtml,
        })
    }

    async #deleteReference(context, dialog = null) {
        if (!await confirmReferenceDeletion())
            return

        try {
            const response = await requestJson("DELETE", `${baseUrl}/${encodeURIComponent(this.bookId)}/ref/${encodeURIComponent(context.ref)}`)
            closeDialog(dialog)
            refreshReferenceOverview()
            dispatchBookReloadNeeded("reference-anchor-deleted", response)
        } catch (error) {
            console.warn("Unable to delete reference", error)
        }
    }

    async handle(book, event) {
        const dialog = document.getElementById("reference-dialog");
        if (!dialog) return

        const context = await this.#loadReference(book, event.detail)
        if (!context)
            return

        if (!hasReferencePreviewHtml(context.targetPreviewHtml) && context.targetNavigationLocation) {
            this.#openReference(context)
            return
        }

        const preview = document.getElementById("referencePreview");
        const sourceInput = dialog.querySelector('[name="source"]');
        if (sourceInput) {
            sourceInput.value = context.source;
        }

        if (!dialog.open) dialog.showModal();
        if (!preview) {
            console.warn('referencePreview element not found');
        } else {
            const previewHtml = context.targetPreviewHtml;
            preview.style.textAlign = 'justify';

            if (previewHtml) {
                // If previewHtml looks like HTML, render it as HTML; otherwise use textContent.
                if (/<[^>]+>/.test(previewHtml)) {
                    preview.innerHTML = `<div style="text-align: justify;">${previewHtml}</div>`;
                } else {
                    preview.textContent = previewHtml;
                }
            } else {
                preview.textContent = 'Keine Vorschau verfügbar';
            }
        }

        setReferenceHeader(
            document.getElementById("referenceHeader"),
            context.sourceAnchorTitle,
            context.reference.target?.shortName ?? context.reference.shortName ?? "Referenz"
        )

        // Preview oder Navigation...
        const openRefBtn = document.getElementById('openReferenceButton');
        if (openRefBtn) {
            openRefBtn.onclick = () => this.#openReference(context, dialog);
        }

        const editRefBtn = document.getElementById('editReferenceButton')
        if (editRefBtn) {
            editRefBtn.onclick = () => this.#editReference(book, context, dialog)
        }

        const deleteRefBtn = document.getElementById('deleteReferenceButton')
        if (deleteRefBtn) {
            deleteRefBtn.onclick = () => this.#deleteReference(context, dialog)
        }
    }

    async showContextMenu(book, detail) {
        const context = await this.#loadReference(book, detail, { createIfMissing: false })
        if (!context)
            return

        globalThis.showSelection({
            type: 'reference',
            ref: context.ref,
            text: context.text,
            title: context.sourceAnchorTitle,
            canOpen: Boolean(context.targetNavigationLocation),
        }).then(action => {
            switch (action) {
                case 'open-reference':
                    this.#openReference(context)
                    break
                case 'edit-reference':
                    this.#editReference(book, context)
                    break
                case 'delete-reference':
                    this.#deleteReference(context)
                    break
            }
        })
    }
}

class Reader {
    autohideCursor
    #cursorAutohider = new CursorAutohider(
        document.documentElement, () => this.autohideCursor)
    #footnoteHandler = new FootnoteHandler()
    #bookReferenceHandler;
    #annotationKeyByValue = new Map()
    #contextImage = null
    style = {
        spacing: 1.4,
        justify: true,
        hyphenate: true,
        invert: false,
    }
    constructor(book, initCfi, bookId, initialLayout) {
        this.book = book
        this.initCfi = initCfi;
        this.bookId = bookId || book.metadata.identifier;
        this.initialLayout = normalizedReaderLayout(initialLayout);
        this.#bookReferenceHandler = new BookReferenceHandler(this.bookId);
        if (book.metadata?.description)
            book.metadata.description = toPangoMarkup(book.metadata.description)
        this.pageTotal = book.pageList
            ?.findLast(x => !isNaN(parseInt(x.label)))?.label
        this.style.mediaActiveClass = book.media?.activeClass

        this.#footnoteHandler.addEventListener('before-render', e => {
            const { view } = e.detail
            view.addEventListener('link', e => {
                e.preventDefault()
                const { href } = e.detail
                this.view.goTo(href)
                footnoteDialog.close()
            })
            view.addEventListener('external-link', e => {
                e.preventDefault()
                dispatch({ type: 'external-link', payload: e.detail })
            })
            footnoteDialog.querySelector('main').replaceChildren(view)

            const { renderer } = view
            renderer.setAttribute('flow', 'scrolled')
            renderer.setAttribute('margin', '12px')
            renderer.setAttribute('gap', '5%')
            renderer.setStyles([
                ...getCSS(this.style),
                `
                body {
                    text-align: justify;
                    hyphens: auto;
                }
                `,
            ])
        })
        //Add the Dialog element in the main.html: footnote-dialog
        this.#footnoteHandler.addEventListener('render', e => {
            const { href, hidden, type } = e.detail

            footnoteDialog.querySelector('[name="href"]').value = href
            footnoteDialog.querySelector('[value="go"]').style.display =
                hidden ? 'none' : 'block'

            const { uiText } = globalThis
            footnoteDialog.querySelector('header').innerText =
                uiText.references[type] ?? uiText.references.footnote
            footnoteDialog.querySelector('[value="go"]').innerText =
                uiText.references[type + '-go'] ?? uiText.references['footnote-go']

            footnoteDialog.showModal()
            dispatch({ type: 'dialog-open' })
        })

    }
    async init() {
        this.view = document.createElement('foliate-view')
        this.view.cfiFilter = cfiFilter
        this.view.height = window.innerHeight;
        this.view.width = window.innerWidth;
        document.body.append(this.view)
        this.sectionFractions = this.view.getSectionFractions()
        await this.view.open(this.book)
        const initialRenderer = this.view?.renderer
        if (initialRenderer && !this.view.isFixedLayout) {
            applyReaderLayout(initialRenderer, this.initialLayout)
        }
        this.#handleEvents()
        await this.view.init({ lastLocation: this.initCfi })
    }
    close() {
        try {
            footnoteDialog.close()
        } catch (e) {
            // The dialog may already be closed or unavailable during teardown.
        }
        try {
            document.getElementById('reference-dialog')?.close()
        } catch (e) {
            // The dialog may already be closed or unavailable during teardown.
        }
        selectionAction?.()
        selectionAction = null
        this.view?.close()
        this.view?.remove()
        this.book?.destroy?.()
        this.view = null
        this.book = null
    }
    async referenceList() {
        const sections = this.book?.sections ?? []
        const references = []
        const seen = new Set()

        for (const [index, section] of sections.entries()) {
            if (!section?.createDocument) continue

            const doc = await section.createDocument()
            const elements = Array.from(doc.querySelectorAll(bookReferenceListSelector))

            for (const el of elements) {
                const ref = referenceIdFromElement(el)
                if (!ref) continue

                const hrefLocation = referenceHrefFromElement(section, el)
                const range = rangeForReferenceElement(doc, el)
                let cfi = ""
                let sectionLabel = section.id ?? ""
                try {
                    if (!hrefLocation)
                        cfi = this.view.getCFI(index, range, cfiFilter)
                    sectionLabel = this.view.getProgressOf(index, range)?.tocItem?.label ?? sectionLabel
                } catch (e) {
                    console.warn(`Unable to locate reference ${ref}`, e)
                } finally {
                    range.detach?.()
                }

                const location = hrefLocation || cfi
                const title = compactText(el.getAttribute('title'))
                const text = compactText(el.textContent)
                const tooltip = title || compactText(el.getAttribute('aria-label')) || text || ref
                const key = `${index}:${ref}:${location}`
                if (seen.has(key)) continue
                seen.add(key)

                references.push({
                    ref,
                    title,
                    text,
                    tooltip,
                    location,
                    section: compactText(sectionLabel),
                    index,
                })
            }
        }

        return references
            .sort((a, b) => a.index - b.index || a.ref.localeCompare(b.ref))
    }
    refreshReferenceOverview() {
        this.referenceList()
            .then(references =>
                dispatch({ type: 'reference-overview', payload: { bookId: this.bookId, references } }))
            .catch(error => {
                console.warn('Unable to build reference overview', error)
                dispatch({ type: 'reference-overview', payload: { bookId: this.bookId, references: [] } })
            })
    }
    async #annotationForDisplay(annotation) {
        const fallbackCfi = annotation?.value || annotation?.cfiRange || annotation?.cfi || ""
        if (fallbackCfi) {
            return {
                ...annotation,
                value: fallbackCfi,
                runtimeCfi: fallbackCfi,
            }
        }

        return null
    }
    #drawAnnotation({ draw, annotation, doc, range }) {
        const { color } = annotation
        if (['underline', 'squiggly', 'strikethrough'].includes(color)) {
            const { defaultView } = doc
            const node = range.startContainer
            const el = node.nodeType === 1 ? node : node.parentElement
            const { writingMode } = defaultView.getComputedStyle(el)

            draw(Overlayer[color], { writingMode, tooltip: annotation.note || "" })
        }
        else draw(Overlayer.highlight, { color, tooltip: annotation.note || "" })
    }
    #removeAnnotationOverlays(annotation) {
        const keys = annotationDrawKeys(annotation)
        if (keys.length === 0) return

        for (const { overlayer } of this.view?.renderer?.getContents?.() ?? []) {
            if (!overlayer) continue
            for (const key of keys)
                overlayer.remove(key)
        }
        for (const key of keys)
            this.#annotationKeyByValue.delete(key)
    }
    #drawAnchoredAnnotation(annotation) {
        const key = annotationDrawKey(annotation)
        if (!key) return false

        const annotationLookupKey = annotationKey(annotation) || key
        let drawn = false
        for (const { doc, overlayer } of this.view?.renderer?.getContents?.() ?? []) {
            if (!doc || !overlayer) continue

            const range = rangeForAnnotationAnchor(doc, annotation)
            if (!range) continue

            overlayer.remove(key)
            const draw = (func, opts) => overlayer.add(key, range, func, opts)
            this.#drawAnnotation({ draw, annotation, doc, range })
            this.#annotationKeyByValue.set(key, annotationLookupKey)
            drawn = true
        }

        return drawn
    }
    #updateAnnotationRuntimeLocation(annotation) {
        const key = annotationKey(annotation)
        if (!key || !annotation?.value) return

        this.#annotationKeyByValue.set(annotation.value, key)
        dispatch({
            type: 'annotation-location',
            payload: {
                bookId: this.bookId,
                annotationId: key,
                anchorId: annotation.anchorId || "",
                value: annotation.value,
                cfiRange: annotation.cfiRange || "",
                runtimeCfi: annotation.value,
                text: annotation.text || "",
            },
        })
    }
    addAnnotation(annotation) {
        this.#annotationForDisplay(annotation)
            .then(resolved => {
                if (this.#drawAnchoredAnnotation(resolved || annotation)) {
                    if (resolved?.value)
                        this.#updateAnnotationRuntimeLocation(resolved)
                    return
                }

                if (!resolved?.value) {
                    console.warn('Unable to resolve annotation anchor', annotation?.annotationId || annotation?.anchorId || annotation?.cfiRange || "")
                    return
                }

                this.#updateAnnotationRuntimeLocation(resolved)
                return this.view.addAnnotation(resolved)
            })
            .catch(error => console.warn('Unable to add annotation', error))
    }
    deleteAnnotation(annotation) {
        this.#removeAnnotationOverlays(annotation)
        this.#annotationForDisplay(annotation)
            .then(resolved => {
                if (!resolved?.value) return
                this.#removeAnnotationOverlays(resolved)
                this.#annotationKeyByValue.delete(resolved.value)
                return this.view.deleteAnnotation(resolved)
            })
            .catch(error => console.warn('Unable to delete annotation', error))
    }
    setContextImageNotInverse() {
        const imageContext = this.#contextImage
        if (!imageContext?.cfi) {
            console.warn("No image context available for not_inverse marker")
            return
        }

        imageContext.element?.classList?.add('not_inverse')
        setImageNotInverse(this.bookId, imageContext)
            .then(response => {
                dispatchBookReloadNeeded("image-not-inverse", response)
            })
            .catch(error => console.warn("Unable to set image not_inverse", error))
    }
    setAppearance({ style, layout, autohideCursor }) {
        Object.assign(this.style, style)
        const { theme } = style
        const $style = document.documentElement.style
        $style.setProperty('--light-bg', theme.light.bg)
        $style.setProperty('--light-fg', theme.light.fg)
        $style.setProperty('--dark-bg', theme.dark.bg)
        $style.setProperty('--dark-fg', theme.dark.fg)
        $style.setProperty('--arianna-reader-background-image', 'none')
        $style.removeProperty('--arianna-reader-background-filter')
        const renderer = this.view?.renderer
        this.view?.setSearchResultColor?.(style.searchResultColor)
        if (renderer) {
            if (style.readerBackgroundImage) {
                renderer.style.setProperty('--arianna-reader-background-image', style.readerBackgroundImage)
                renderer.style.setProperty('--arianna-reader-background-filter', style.readerBackgroundFilter ?? 'none')
            } else {
                renderer.style.setProperty('--arianna-reader-background-image', 'none')
                renderer.style.removeProperty('--arianna-reader-background-filter')
            }
            applyReaderLayout(renderer, layout)
            renderer.setStyles?.(getCSS(this.style))
        }
        document.body.classList.toggle('invert', this.style.invert)
        this.autohideCursor = autohideCursor
    }
    #handleEvents() {
        this.view.addEventListener('relocate', e => {
            const { heads, feet } = this.view.renderer
            if (heads?.length > 0) {
                const { tocItem } = e.detail
                heads.at(-1).innerText = tocItem?.label ?? ''
                if (heads.length > 1)
                    heads[0].innerText = this.book.metadata.title
            }
            if (feet?.length > 0) {
                const { pageItem, location: { current, next, total } } = e.detail
                if (pageItem) {
                    // only show page number at the end
                    // because we only have visible range for the spread,
                    // not each column
                    feet.at(-1).innerText = format.page(pageItem.label, this.pageTotal)
                    if (feet.length > 1)
                        feet[0].innerText = format.loc(current + 1, total)
                }
                else {
                    feet[0].innerText = format.loc(current + 1, total)
                    if (feet.length > 1) {
                        const r = 1 - 1 / feet.length
                        const end = Math.floor((1 - r) * current + r * next)
                        feet.at(-1).innerText = format.loc(end + 1, total)
                    }
                }
            }
            dispatch({
                type: 'relocate',
                payload: {
                    fraction: e.detail.fraction,
                    section: e.detail.section,
                    location: e.detail.location,
                    time: e.detail.time,
                    tocItem: e.detail.tocItem,
                    pageItem: e.detail.pageItem,
                    cfi: e.detail.cfi
                }
            });
        })
        this.view.addEventListener('create-overlay', e =>
            dispatch({ type: 'create-overlay', payload: e.detail }))
        this.view.addEventListener('show-annotation', e => {
            const { value, index, range } = e.detail
            const pos = getPosition(range)
            const annotationKey = this.#annotationKeyByValue.get(value) || value
            this.#showAnnotation({ index, range, value, annotationKey, pos })
        })
        this.view.addEventListener('draw-annotation', e => this.#drawAnnotation(e.detail))
        this.view.addEventListener('external-link', e => {
            e.preventDefault()
            dispatch({ type: 'external-link', payload: e.detail })
        })
        this.view.addEventListener('link', e =>
            this.#footnoteHandler.handle(this.book, e)?.catch(err => {
                console.warn(err)
                this.view.goTo(e.detail.href)
            }))
        this.view.addEventListener('book-reference', e =>
            this.#bookReferenceHandler.handle(this.book, e)
                ?.catch(err => console.warn(err))
        )
        this.view.addEventListener('load', e => this.#onLoad(e))
        this.view.history.addEventListener('index-change', e => {
            const { canGoBack, canGoForward } = e.target
            dispatch({ type: 'history-index-change', payload: { canGoBack, canGoForward } })
        })
    }
    #onLoad(e) {
        const { doc, index } = e.detail

        doc.addEventListener('dblclick', event => {
            const img = event.target.closest?.('img')
            if (!img) return
            fetch(img.src)
                .then(res => res.blob())
                .then(blob => Promise.all([blobToBase64(blob), blob.type]))
                .then(([base64, mimetype]) =>
                    dispatch({ type: 'show-image', payload: { base64, mimetype } }))
                .catch(e => console.error(e))
        })

        doc.addEventListener('contextmenu', event => {
            const referenceEl = event.target.closest?.(bookReferenceClickSelector)
            if (referenceEl) {
                const ref = referenceIdFromElement(referenceEl)
                if (ref) {
                    event.preventDefault()
                    event.stopPropagation()
                    this.#contextImage = null
                    this.#bookReferenceHandler.showContextMenu(this.book, {
                        ref,
                        text: referenceEl.textContent ?? "",
                        title: referenceEl.getAttribute("title") ?? "",
                        role: referenceEl.dataset.role,
                        anchorType: referenceEl.dataset.anchorType,
                    }).catch(err => console.warn(err))
                    return
                }
            }

            const img = event.target.closest?.('img')
            if (!img) {
                this.#contextImage = null
                return
            }

            try {
                this.#contextImage = {
                    element: img,
                    cfi: this.view.getCFI(index, rangeForNode(img), cfiFilter),
                    src: img.getAttribute('src') || img.currentSrc || img.src || "",
                }
            } catch (e) {
                this.#contextImage = null
                console.warn("Unable to create image CFI for context menu", e)
            }
        })

        let isSelecting = false
        doc.addEventListener('pointerdown', () => isSelecting = true)
        doc.addEventListener('pointerup', () => {
            isSelecting = false
            const sel = doc.getSelection()
            const range = getSelectionRange(sel)
            if (!range) return
            const pos = getPosition(range)
            const value = this.view.getCFI(index, range, cfiFilter)
            const lang = getLang(range.commonAncestorContainer)
            const text = sel.toString()
            this.#showSelection({ index, range, lang, value, pos, text })
        })

        if (!this.view.isFixedLayout)
            // go to the next page when selecting to the end of a page
            // this makes it possible to select across pages
            doc.addEventListener('selectionchange', debounce(() => {
                if (!isSelecting) return
                if (!selectionAutoTurnEnabled(this.view)) return
                if (this.view.renderer.getAttribute('flow') !== 'paginated') return
                const { lastLocation } = this.view
                if (!lastLocation) return
                const selRange = getSelectionRange(doc.getSelection())
                if (!selRange) return
                if (selRange.compareBoundaryPoints(Range.END_TO_END, lastLocation.range) >= 0)
                    this.view.next()
            }, 1000))
        doc.addEventListener('click', event => {
            const el = event.target.closest?.(bookReferenceClickSelector)
            if (!el) return
            const ref = referenceIdFromElement(el)
            if (!ref) return

            event.preventDefault()
            event.stopPropagation()

            this.view.dispatchEvent(new CustomEvent('book-reference', {
                detail: {
                    ref,
                    text: el.textContent ?? "",
                    title: el.getAttribute("title") ?? "",
                    role: el.dataset.role,
                    anchorType: el.dataset.anchorType
                }
            }))
        })
        this.#cursorAutohider.cloneFor(doc.documentElement)
    }
    #showAnnotation({ index, range, value, annotationKey, pos }) {
        globalThis.showSelection({ type: 'annotation', value, annotationKey: annotationKey || value, cfiRange: value, pos })
            .then(action => {
                if (action === 'select')
                    this.#showSelection({ index, range, value, pos })
            })
    }
    #showSelection({ index, range, lang, value, pos, text }) {
        if (!text) {
            const sel = range.startContainer.ownerDocument.getSelection()
            sel.removeAllRanges()
            sel.addRange(range)
            text = sel.toString()
        }
        const content = range.toString()
        const selectionProgress = this.view.getProgressOf(index, range)
        globalThis.showSelection({
            type: 'selection',
            text,
            content,
            lang,
            value,
            pos,
            ...selectionProgress,
        }).then(action => {
            switch (action) {
                case 'reference':
                    console.log("Reference selected:", this.bookId, text, content, lang, value, pos);
                    createReference(this.bookId, value, text, success => {
                        if (success) console.log("Reference stored.");
                        else console.log("Reference not stored");
                    });
                    break;
                case 'copy': getHTML(range).then(html =>
                    dispatch({ type: 'selection', payload: { action, text, html } }))
                    break
                case 'find':
                    dispatch({ type: 'selection-find', payload: { text } })
                    break
                case 'translate':
                    dispatch({ type: 'selection', payload: { action, text, pos } })
                    break
                case 'copy-citation':
                    dispatch({
                        type: 'selection', payload: {
                            action, text, value,
                            ...this.view.getProgressOf(index, range)
                        }
                    })
                    break
                case 'highlight':
                    this.#showAnnotation({ index, range, value, pos })
                    break
                case 'print':
                    this.printRange(range.startContainer.ownerDocument, range)
                    break
                case 'speak-from-here':
                    this.view.initTTS().then(() => dispatch({
                        type: 'selection', payload: {
                            action,
                            ssml: this.view.tts.from(range),
                        }
                    }))
                    break
            }
        })
    }

    createTocMap() {
        const map = new Map();
        const processTocItem = (item) => {
            const { index } = this.book.resolveHref(item.href);
            if (index !== undefined) {
                map.set(index, item.label);
            }
            // nested chapters/sections
            if (item.subitems) {
                item.subitems.forEach(processTocItem);
            }
        };
        return map;
    }

    async search(query, options = {}) {
        const results = [];

        for await (const result of this.view.search({ query, ...options })) {
            if (result === 'done' || result.progress) {
                continue;
            }

            if (result.subitems) {
                for (const item of result.subitems) {
                    results.push({
                        cfi: item.cfi,
                        excerpt: item.excerpt,
                        section: result.label
                    });
                }
            } else if (result.cfi) {
                results.push({
                    cfi: result.cfi,
                    excerpt: result.excerpt,
                    section: ''
                });
            }
        }

        dispatch({ type: 'find-results', payload: { query, results } });
        return results;
    }

    printRange(doc, range) {
        const iframe = document.createElement('iframe')
        // NOTE: it needs `allow-scripts` to remove the frame after printing
        // and `allow-modals` to show the print dialog
        iframe.setAttribute('sandbox', 'allow-same-origin allow-scripts allow-modals')
        const css = getCSS(this.style)
        iframe.addEventListener('load', () => {
            const doc = iframe.contentDocument

            const beforeStyle = doc.createElement('style')
            beforeStyle.textContent = css[0]
            doc.head.prepend(beforeStyle)

            const afterStyle = doc.createElement('style')
            afterStyle.textContent = css[1]
            doc.head.append(afterStyle)

            if (range) {
                const frag = range.cloneContents()
                doc.body.replaceChildren()
                doc.body.appendChild(frag)
            }
            iframe.contentWindow.addEventListener('afterprint', () =>
                iframe.remove())
            iframe.contentWindow.print()
        }, { once: true })

        iframe.src = doc.defaultView.frameElement.src
        iframe.style.display = 'none'
        document.body.append(iframe)
    }
    print() {
        this.printRange(this.view.renderer.getContents()[0]?.doc)
    }
    async getCover() {
        try {
            const blob = await this.book.getCover?.()
            return blob ? blobToBase64(blob) : null
        } catch (e) {
            console.warn(e)
            console.warn('Failed to load cover')
            return null
        }
    }

    // wrap these renderer methods
    // because `FoliateWebView.exec()` can only pass one argument
    scrollBy([x, y]) {
        return this.view.renderer.scrollBy?.(x, y)
    }
    snap([x, y]) {
        return this.view.renderer.snap?.(x, y)
    }
}

globalThis.find = {
    find: (query, inBook, highlight) => globalThis.reader.search(query),
    clearHighlight: () => globalThis.reader.view.clearSearch(),
}

const printf = (str, args) => {
    for (const arg of args) str = str.replace('%s', arg)
    return str
}

globalThis.init = ({ uiText }) => {
    globalThis.uiText = uiText

    format.loc = (a, b) => printf(uiText.loc, [a, b])
    format.page = (a, b) => b
        ? printf(uiText.page, [a, b])
        : printf(uiText.pageWithoutTotal, [a])

    footnoteDialog.querySelector('[value="close"]').innerText = uiText.close
}
