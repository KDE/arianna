// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Read-only DOM capture, independent of speech, translation and Codex. Offsets are
// UTF-16, matching DOM Range/QString. Never clone into or modify the EPUB document.
const elementOf = node => node?.nodeType === 1 ? node : node?.parentElement
const ignored = node => elementOf(node)?.closest('script,style,noscript,mjx-container,[data-arianna-generated]')
const languageOf = (node, fallback) => {
    for (let element = elementOf(node); element; element = element.parentElement) {
        if (element.hasAttributeNS('http://www.w3.org/XML/1998/namespace', 'lang'))
            return element.getAttributeNS('http://www.w3.org/XML/1998/namespace', 'lang')
        if (element.hasAttribute('lang')) return element.getAttribute('lang')
        if (element.hasAttribute('xml:lang')) return element.getAttribute('xml:lang')
    }
    return fallback ?? ''
}
const textNodes = root => {
    if (root.nodeType === 3) return [root]
    const walker = root.ownerDocument.createTreeWalker(root, NodeFilter.SHOW_TEXT)
    const nodes = []
    for (let node = walker.nextNode(); node; node = walker.nextNode()) nodes.push(node)
    return nodes
}
const sourceText = element => textNodes(element).filter(node => !ignored(node)).map(node => node.data).join('')
const verseOf = node => {
    for (let element = elementOf(node); element; element = element.parentElement) {
        const values = new Set()
        for (const attribute of ['data-verse', 'data-verse-number']) {
            if (element.hasAttribute(attribute)) {
                const value = element.getAttribute(attribute).trim()
                if (!/^[\p{Nd}]+(?:[.:/-][\p{Nd}]+)*(?:[a-z])?$/u.test(value)) return null
                values.add(value)
            }
        }
        if (values.size) return values.size === 1 ? [...values][0] : null
    }
    return null
}

export const collectReaderSelectionContext = (range, { bookLanguage } = {}) => {
    if (!range || range.collapsed) return {}
    const doc = range.startContainer.ownerDocument
    const selected = []
    let offset = 0
    for (const node of textNodes(range.commonAncestorContainer)) {
        if (!range.intersectsNode(node)) continue
        const start = node === range.startContainer ? range.startOffset : 0
        const end = node === range.endContainer ? range.endOffset : node.length
        const text = node.data.slice(start, end)
        if (!text) continue
        const language = languageOf(node, bookLanguage)
        const previous = selected.at(-1)
        if (previous && previous.language === language && previous.end === offset) {
            previous.text += text
            previous.end += text.length
        } else selected.push({ text, language, start: offset, end: offset + text.length })
        offset += text.length
    }
    const result = { languageSegments: selected }
    const languages = new Set(selected.map(item => item.language))
    // Empty lang is authoritative; a mixed-language selection has no single lang.
    if (languages.size === 1) result.language = [...languages][0]
    else if (languages.size > 1) result.language = 'mul'

    const startElement = elementOf(range.startContainer)
    const endElement = elementOf(range.endContainer)
    const blockSelector = 'p,li,blockquote,pre,dd,dt,h1,h2,h3,h4,h5,h6'
    const first = startElement?.closest(blockSelector)
    const last = endElement?.closest(blockSelector)
    const paragraph = first && last ? (first === last ? first : elementOf(range.commonAncestorContainer)) : first || last
    const bounded = (key, text, limit) => {
        if (!text) return
        // Do not split a surrogate pair when bounding optional surroundings.
        let end = Math.min(text.length, limit)
        if (end < text.length && /[\uD800-\uDBFF]/.test(text[end - 1])) --end
        result[key] = text.slice(0, end)
        if (end < text.length) result[`${key}Truncated`] = true
    }
    if (paragraph) {
        const text = sourceText(paragraph)
        bounded('paragraph', text, 12000)
        if (first === last && typeof Intl.Segmenter === 'function') {
            let start = 0, end = 0, cursor = 0
            for (const node of textNodes(paragraph)) {
                if (ignored(node)) continue
                if (node === range.startContainer) start = cursor + range.startOffset
                else if (range.startContainer.nodeType === 1 && range.comparePoint(node, 0) < 0) start = cursor + node.length
                if (node === range.endContainer) end = cursor + range.endOffset
                cursor += node.length
            }
            // Element boundaries: comparePoint determines which text nodes overlap.
            if (range.endContainer.nodeType === 1) {
                cursor = 0
                for (const node of textNodes(paragraph)) {
                    if (ignored(node)) continue
                    if (range.comparePoint(node, 0) <= 0) end = cursor + node.length
                    cursor += node.length
                }
            }
            const language = selected[0]?.language
            let segmenter
            try { segmenter = new Intl.Segmenter(language || undefined, { granularity: 'sentence' }) }
            catch { segmenter = new Intl.Segmenter(undefined, { granularity: 'sentence' }) }
            const sentences = [...segmenter.segment(text)].filter(item => item.index < end && item.index + item.segment.length > start)
            bounded('sentence', sentences.map(item => item.segment).join(''), 12000)
        }
    }
    const section = startElement?.closest('section,article') || doc.body
    if (section) bounded('section', sourceText(section), 24000)

    for (const heading of doc.querySelectorAll('h1,h2,h3,h4,h5,h6')) {
        if (heading.contains(range.startContainer) || (heading.compareDocumentPosition(range.startContainer) & Node.DOCUMENT_POSITION_FOLLOWING))
            result.heading = sourceText(heading)
        else break
    }
    const verses = new Set()
    let allHaveVerse = true
    for (const node of textNodes(range.commonAncestorContainer)) {
        if (!range.intersectsNode(node) || ignored(node)) continue
        const start = node === range.startContainer ? range.startOffset : 0
        const end = node === range.endContainer ? range.endOffset : node.length
        if (!node.data.slice(start, end).trim()) continue
        const verse = verseOf(node)
        if (verse) verses.add(verse)
        else allHaveVerse = false
    }
    if (allHaveVerse && verses.size === 1) result.verse = [...verses][0]
    return result
}
