// SPDX-FileCopyrightText: John Factotum
// SPDX-License-Identifier: MIT

// DOM block traversal adapted from foliate-js for native speech extraction.
const blockTags = new Set([
    'article', 'aside', 'audio', 'blockquote', 'caption',
    'details', 'dialog', 'div', 'dl', 'dt', 'dd',
    'figure', 'footer', 'form', 'figcaption',
    'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'header', 'hgroup', 'hr', 'li',
    'main', 'math', 'nav', 'ol', 'p', 'pre', 'section', 'tr',
])

const rangeIsEmpty = range => !range.toString().trim()

export function* getBlocks(doc, { math = false } = {}) {
    let last = doc.createRange()
    last.setStart(doc.body, 0)
    const walker = doc.createTreeWalker(doc.body, NodeFilter.SHOW_ELEMENT, math ? {
        acceptNode: node => node.parentElement?.closest('mjx-container')
            ? NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT,
    } : null)
    for (let node = walker.nextNode(); node; node = walker.nextNode()) {
        const name = node.tagName.toLowerCase()
        if (blockTags.has(name) || (math && name === 'mjx-container' && node.getAttribute('display') === 'true')) {
            if (last) {
                last.setEndBefore(node)
                if (math || !rangeIsEmpty(last)) yield last
            }
            last = doc.createRange()
            last.setStart(node, 0)
        }
    }
    last.setEndAfter(doc.body.lastChild ?? doc.body)
    if (math || !rangeIsEmpty(last)) yield last
}
