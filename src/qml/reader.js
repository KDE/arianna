// SPDX-FileCopyrightText: John Factotum
// SPDX-License-Identifier: MIT

import './foliate-js/view.js'
import { FootnoteHandler } from './foliate-js/footnotes.js'
import { waitForMath } from './foliate-js/mathjax.js'
import { createFootnoteAppearance, createFootnotePreview } from './footnote-preview.js'
import { toPangoMarkup } from './markup.js'
import { getBlocks } from './speech-blocks.js'
import { semanticPath, normalizedDigits } from './work-structure.js'
import { collectReaderSelectionContext } from './reader-context.js'
import { Overlayer } from './foliate-js/overlayer.js'
import { installReaderInteractions, readerInteractionCSS } from './reader-interactions.js'
import { compare as compareCFI, isCFI } from './foliate-js/epubcfi.js'

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

// Native keypad events can be consumed by WebEngine before QML shortcuts run.
// Handle them in both the reader shell and the loaded EPUB frames.
const handlePageWidthKey = event => {
    if (event.ctrlKey || event.altKey || event.metaKey || event.shiftKey) return
    const add = event.code === 'NumpadAdd'
        || (event.location === 3 && event.key === '+')
    const subtract = event.code === 'NumpadSubtract'
        || (event.location === 3 && event.key === '-')
    if ((!add && !subtract) || !globalThis.reader?.view) return
    const target = event.target
    if (target?.isContentEditable || target?.closest?.('input, textarea, select')
        || document.querySelector('dialog[open]')
        || target?.ownerDocument?.querySelector('dialog[open]')) return
    event.preventDefault()
    event.stopPropagation()
    dispatch({ type: 'change-page-width', payload: {
        delta: add ? 25 : -25,
        source: `WebEngine ${event.code || event.key}`,
    } })
}
document.addEventListener('keydown', handlePageWidthKey, true)

let selectionAction
let selectionRequest = 0
globalThis.showSelection = detail => new Promise(resolve => {
    selectionAction?.()
    selectionAction = resolve
    dispatch({ type: 'show-selection', payload: {...detail, requestId: ++selectionRequest} })
})

globalThis.selectionAction = (action, requestId) => {
    if (requestId !== undefined && requestId !== selectionRequest) return
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

const normalizeLanguageTag = value => {
    if (typeof value !== 'string' || !value.trim()) return ''
    try {
        return Intl.getCanonicalLocales(value.trim().replaceAll('_', '-'))[0] ?? ''
    } catch {
        return ''
    }
}

const getLang = node => {
    // An explicit empty language means unknown and stops inheritance.
    for (let el = node?.nodeType === 1 ? node : node?.parentElement; el; el = el.parentElement) {
        if (el.hasAttributeNS('http://www.w3.org/XML/1998/namespace', 'lang'))
            return normalizeLanguageTag(el.getAttributeNS('http://www.w3.org/XML/1998/namespace', 'lang'))
        if (el.hasAttribute('lang')) return normalizeLanguageTag(el.getAttribute('lang'))
        if (el.hasAttribute('xml:lang')) return normalizeLanguageTag(el.getAttribute('xml:lang'))
    }
}

const bookLanguageHint = languages => {
    for (const value of Array.isArray(languages) ? languages : [languages]) {
        const language = normalizeLanguageTag(value)
        if (language) return language
    }
    return ''
}

// Reuse the EPUB's MathJax instance. In v4, the accessibility worker attaches
// speech asynchronously after typesetting; never load a second instance here.
const pendingMathSpeech = new WeakMap()
const awaitMathSpeechReady = (doc, range) => {
    if (doc.defaultView?.ariannaMathReady) return doc.defaultView.ariannaMathReady
    // Copying/selecting ordinary prose must not wait for an unrelated CDN.
    if (range && !speechMathContainers(range).length) return Promise.resolve()
    if (pendingMathSpeech.has(doc)) return pendingMathSpeech.get(doc)
    const win = doc.defaultView
    const script = doc.querySelector('script#MathJax-script')
        ?? Array.from(doc.querySelectorAll('script[src]')).find(el => /mathjax/i.test(el.getAttribute('src')))
    if (!win?.MathJax && !script) return Promise.resolve()
    const promise = new Promise(resolve => {
        let timer, poll
        let waiting = false
        let finished = false
        const finish = () => {
            if (finished) return
            finished = true
            clearTimeout(timer)
            clearInterval(poll)
            script?.removeEventListener('error', failed)
            resolve()
        }
        const failed = () => {
            if (finished) return
            console.warn('MathJax speech preparation failed; using available formula labels')
            finish()
        }
        const check = async () => {
            if (waiting || finished) return
            const mathjax = win.MathJax
            if (!mathjax?.startup?.promise && typeof mathjax?.whenReady !== 'function') return
            waiting = true
            try {
                await mathjax.startup?.promise
                if (typeof mathjax.whenReady === 'function') await mathjax.whenReady(() => {})
                finish()
            } catch { failed() }
        }
        timer = setTimeout(failed, 10000)
        script?.addEventListener('error', failed, { once: true })
        poll = setInterval(check, 50)
        check()
    }).finally(() => pendingMathSpeech.delete(doc))
    pendingMathSpeech.set(doc, promise)
    return promise
}

const preparedMathSpeech = new WeakMap()
// Reader-only aliases never become attributes or text in the book DOM.
const preparedReferenceSpeech = new WeakMap()
let speechVoiceLocale = ''
globalThis.setSpeechOptions = options => {
    speechVoiceLocale = normalizeLanguageTag(options?.locale) || ''
}
let mathSpeechOptions = { locale: '', concise: true }
globalThis.setMathSpeechOptions = options => {
    const locale = normalizeLanguageTag(options?.locale) || ''
    const concise = options?.concise !== false
    if (locale !== mathSpeechOptions.locale || concise !== mathSpeechOptions.concise)
        mathSpeechOptions = { locale, concise }
}
const prepareMathSpeech = async (doc, range) => {
    await awaitMathSpeechReady(doc, range)
    const generate = doc.defaultView?.ariannaMathSpeech
    if (typeof generate !== 'function') return
    const options = mathSpeechOptions
    const formulas = range ? speechMathContainers(range) : Array.from(doc.querySelectorAll('mjx-container'))
    await Promise.all(formulas.map(async formula => {
        preparedMathSpeech.delete(formula)
        try {
            const speech = await generate(formula, options)
            if (options === mathSpeechOptions && speech?.text && normalizeLanguageTag(speech.locale))
                preparedMathSpeech.set(formula, speech)
        } catch (error) {
            console.warn('Arianna formula speech preparation failed; using available labels', error)
        }
    }))
    // A voice change while the worker replies must not restore an older locale.
    if (options !== mathSpeechOptions) await prepareMathSpeech(doc, range)
}

const speechMathContainers = range => {
    const root = range.commonAncestorContainer
    const el = root.nodeType === 1 ? root : root.parentElement
    const enclosing = el?.closest('mjx-container')
    const candidates = enclosing ? [enclosing] : Array.from(el?.querySelectorAll('mjx-container') ?? [])
    return candidates.filter(node => !node.parentElement?.closest('mjx-container') && range.intersectsNode(node))
}

const speechReferenceAnchors = range => {
    const root = range.commonAncestorContainer
    const el = root.nodeType === 1 ? root : root.parentElement
    const selector = `${crossReferenceAnchorSelector}, ${legacyBookReferenceSelector}`
    const enclosing = el?.closest(selector)
    const candidates = enclosing ? [enclosing] : Array.from(el?.querySelectorAll(selector) ?? [])
    return candidates.filter(node => range.intersectsNode(node))
}

const spokenReferenceTitle = title => compactText(title).replace(/[\p{L}\p{M}]+/gu, word => {
    // Publisher titles in capitals otherwise invite letter-by-letter speech.
    // Roman volume numbers retain their conventional spelling.
    if (word.length < 2 || word !== word.toLocaleUpperCase() || /^[IVXLCDM]+$/.test(word)) return word
    return word[0] + word.slice(1).toLocaleLowerCase()
})

const speechReferenceLabel = (anchor, bookLanguages) => {
    const reference = preparedReferenceSpeech.get(anchor)
    const hint = compactText(reference?.hint ?? anchor.getAttribute('title'))
    if (!hint) return null
    const lang = getLang(anchor) ?? bookLanguageHint(bookLanguages) ?? ''
    const locale = speechVoiceLocale || lang
    const normalized = hint.match(/^.+?\s+(Kapitel|Vers|Gleichung)\s+(.+)$/iu)
    const title = spokenReferenceTitle(reference?.title || '')
    if (!normalized || !title) return { text: hint, lang }
    const kind = normalized[1].toLocaleLowerCase()
    const names = {
        de: { kapitel: 'Kapitel', vers: 'Vers', gleichung: 'Gleichung' },
        en: { kapitel: 'Chapter', vers: 'Verse', gleichung: 'Equation' },
        sa: { kapitel: 'अध्याय', vers: 'श्लोक', gleichung: 'समीकरण' },
    }
    const label = names[locale.split('-')[0]]?.[kind] || normalized[1]
    return { text: `${title}, ${label} ${normalized[2]}`, lang }
}

const atomicSpeechPart = segment => segment.type === 'math' || segment.type === 'reference' || segment.type === 'coordinate'

const visibleSpeechMath = node => {
    for (let el = node; el; el = el.parentElement) {
        const style = node.ownerDocument.defaultView?.getComputedStyle(el)
        if (style?.display === 'none' || style?.contentVisibility === 'hidden') return false
    }
    const style = node.ownerDocument.defaultView?.getComputedStyle(node)
    return style?.visibility !== 'hidden' && style?.visibility !== 'collapse'
}

const speechMathText = formula => {
    const prepared = preparedMathSpeech.get(formula)?.text
    if (prepared) return prepared
    // Some v4 bundles also expose an unmarked label. Never pass their embedded
    // SSML index marks to the native plain-text utterance for this atomic formula.
    const plain = formula.getAttribute('data-semantic-speech-none')?.trim()
    if (plain) return plain
    const label = formula.getAttribute('data-semantic-speech')?.trim() ?? ''
    if (!/<\/?(?:speak|mark|say-as|prosody|sub|phoneme|break|voice|lang|emphasis|p|s)(?:\s|\/?>)/i.test(label)) return label
    const parsed = new DOMParser().parseFromString(`<speak>${label}</speak>`, 'application/xml')
    if (parsed.querySelector('parsererror')) {
        console.warn('MathJax formula has invalid speech markup')
        return ''
    }
    return parsed.documentElement.textContent.replace(/\s+/g, ' ').trim()
}

const speechBlockFor = node => (node.nodeType === 1 ? node : node.parentElement)?.closest(
    'p,h1,h2,h3,h4,h5,h6,li,dt,dd,blockquote,pre,div,td,th,figcaption,section,article,aside,header,footer,body')

const speechBlockId = (block, locateRange) => {
    // Keep the anchor inside the block, including the EPUB's <body> root.
    const source = block.ownerDocument.createRange()
    source.selectNodeContents(block)
    source.collapse(true)
    return locateRange(source)
}

const speechMathSymbolCount = formula => {
    // Count MathML tokens once, not TeX commands, spoken words or SVG glyphs.
    // A number/identifier is one token; a fraction bar or radical adds one.
    const weight = (kind, text) => ['mi', 'mn', 'mo', 'mglyph'].includes(kind)
        ? (kind === 'mglyph' || text.replace(/[\s\u2061-\u2064]/gu, '') ? 1 : 0)
        : ['mfrac', 'msqrt', 'mroot'].includes(kind) ? 1 : kind === 'mfenced' ? 2 : 0
    // Detached reader conversions reuse a MathItem; its current root may be a
    // later formula. Count the captured expanded source for this expression.
    const captured = preparedMathSpeech.get(formula)?.mathml
        || (formula.hasAttribute('data-arianna-math-output')
            && formula.ownerDocument.defaultView?.ariannaMathDetails?.(formula)?.mathml)
    if (captured) {
        const parsed = new DOMParser().parseFromString(captured, 'application/xml')
        if (!parsed.querySelector('parsererror')) return Array.from(parsed.querySelectorAll('*'))
            .reduce((count, node) => count + weight(node.localName, node.textContent), 0)
    }
    const math = formula.ownerDocument.defaultView?.MathJax?.startup?.document?.math
    if (math?.[Symbol.iterator]) {
        for (const item of math) {
            if (item.typesetRoot !== formula && !formula.contains(item.typesetRoot)) continue
            if (!item.root?.walkTree) break
            let count = 0
            item.root.walkTree(node => { count += weight(node.kind, node.getText?.() ?? '') })
            return count
        }
    }
    // Prepared output may be stored in the book without a live MathJax instance.
    const svg = formula.querySelector(':scope > svg')
    const chtml = formula.querySelector(':scope > mjx-math')
    const mathml = formula.querySelector('mjx-assistive-mml math')
    const nodes = svg ? Array.from(svg.querySelectorAll('[data-mml-node]'))
        : chtml ? Array.from(chtml.querySelectorAll('mjx-mi,mjx-mn,mjx-mo,mjx-mglyph,mjx-mfrac,mjx-msqrt,mjx-mroot,mjx-mfenced')) : []
    if (nodes.length) return nodes.reduce((count, node) => {
        const kind = node.getAttribute('data-mml-node') ?? node.localName.slice(4)
        // SVG token wrappers contain <use> elements rather than literal text.
        const text = node.textContent || node.getAttribute('data-semantic-text')
            || (node.querySelector('use,path,mjx-c') ? 'glyph' : '')
        return count + weight(kind, text)
    }, 0)
    if (mathml) return Array.from(mathml.querySelectorAll('*'))
        .reduce((count, node) => count + weight(node.localName, node.textContent), 0)
    return null
}

// Detect sentence boundaries on the paragraph with atomic placeholders for
// inline formulas. Punctuation in a formula's speech label cannot split prose.
const splitSpeechContent = (segments, locateRange, sourceKey) => {
    const result = []
    let group = []
    const flush = () => {
        if (!group.length) return
        const text = group.map(segment => atomicSpeechPart(segment) ? '\uFFFC' : segment.text).join('')
        const segmentationText = text.replace(
            /(?<![\p{L}\p{M}\p{N}])(?:[0-9]+\.(?=\s+[\p{L}])|i\s*\.\s*d\s*\.\s*r\.?|z\s*\.\s*b\.?|z\s*\.\s*t\.?|u\s*\.\s*a\.?|u\s*\.\s*ä\.?|d\s*\.\s*h\.?|bzw\.?|usw\.?|etc\.?|ca\.?|nr\.?|dr\.?)(?![\p{L}\p{M}])/giu,
            (abbreviation, offset) => {
                const after = text.slice(offset + abbreviation.length)
                const endsQuotedSentence = /^["'’”»›)\]]+\s+(?=[\p{Lu}\p{N}])/u.test(after)
                const finalPeriod = abbreviation.lastIndexOf('.')
                return abbreviation.replace(/\./g, (period, index) =>
                    endsQuotedSentence && index === finalPeriod ? period : '\uE000')
            })
        const sentences = new Intl.Segmenter(group[0].lang || undefined, { granularity: 'sentence' })
        const groupId = `${speechBlockId(group[0].block, locateRange)}:${sourceKey}`
        const spans = []
        let offset = 0
        for (const segment of group) {
            const length = atomicSpeechPart(segment) ? 1 : segment.text.length
            spans.push({ segment, start: offset, end: offset + length })
            offset += length
        }
        let sentence = 0
        for (const { index, segment: sentenceText } of sentences.segment(segmentationText)) {
            const end = index + sentenceText.length
            const sentenceId = `${groupId}:${sentence++}`
            for (const span of spans) {
                const start = Math.max(index, span.start)
                const stop = Math.min(end, span.end)
                if (stop <= start) continue
                const part = span.segment
                const begin = start - span.start
                const finish = stop - span.start
                result.push(atomicSpeechPart(part) ? { ...part, sentenceId } : {
                    ...part, sentenceId, text: part.text.slice(begin, finish),
                    sourceMap: sliceSpeechSourceMap(part.sourceMap, begin, finish),
                })
            }
        }
        group = []
    }
    for (const segment of segments) {
        if ((segment.type === 'math' && !segment.inline) || !segment.block) {
            flush()
            result.push(segment)
            continue
        }
        const previous = group.at(-1)
        if (previous && (previous.block !== segment.block || previous.lang !== segment.lang)) flush()
        group.push(segment)
    }
    flush()
    return result
}

// MathJax's generated glyph text and assistive MathML are alternate renderings
// of one formula. Formulas and crossrefs keep their original source ranges
// while contributing an atomic, reader-only speech label.
const preparedCoordinateSpeech = new WeakMap()
const coordinateSpeechSegments = (segments, locateRange) => {
    const result = []
    for (const segment of segments) {
        if (atomicSpeechPart(segment) || !segment.sourceMap?.length || !/[0-9०-९]+[.:-][0-9०-९]+/.test(segment.text)) { result.push(segment); continue }
        let copied = 0
        for (const match of segment.text.matchAll(/[0-9०-९]+(?:[.:-][0-9०-९]+)+/g)) {
            const start = match.index, end = start + match[0].length
            const spans = sliceSpeechSourceMap(segment.sourceMap, start, end)
            if (!spans.length || spans.some(span => !span.exact)) continue
            const ancestor = node => {
                for (let element = node.parentElement; element; element = element.parentElement) {
                    if (preparedCoordinateSpeech.has(element)) return preparedCoordinateSpeech.get(element)
                }
                return null
            }
            const descriptor = ancestor(spans[0].node)
            if (!descriptor || spans.some(span => ancestor(span.node) !== descriptor)
                || normalizedDigits(match[0]) !== normalizedDigits(descriptor.marker)) continue
            // Only the source marker at the end of its verse qualifies. A
            // repeated coordinate in the verse's prose is not a marker.
            const blockText = spans[0].node.parentElement.closest('p,div')?.textContent || ''
            const terminal = blockText.match(/॥\s*([0-9०-९]+(?:[.:-][0-9०-९]+)+)\s*॥\s*$/)
            if (!terminal || terminal[1] !== match[0]) continue
            const block = spans[0].node.parentElement.closest('p,div')
            const trailing = spans[0].node.ownerDocument.createRange()
            trailing.selectNodeContents(block)
            trailing.setStart(spans.at(-1).node, spans.at(-1).end)
            if (!/^\s*॥\s*$/.test(trailing.toString())) continue
            if (start > copied) result.push({...segment, text: segment.text.slice(copied, start), sourceMap: sliceSpeechSourceMap(segment.sourceMap, copied, start)})
            const source = spans[0].node.ownerDocument.createRange()
            source.setStart(spans[0].node, spans[0].start)
            source.setEnd(spans.at(-1).node, spans.at(-1).end)
            const cfi = locateRange(source)
            result.push({...segment, type: 'coordinate', inline: true, text: match[0], cfi,
                structureCoordinate: descriptor.semanticPath,
                marks: [{start: 0, length: match[0].length, cfi}],
                sourceMap: [{spokenStart: 0, spokenLength: match[0].length, cfi, exact: true}]})
            copied = end
        }
        if (!copied) result.push(segment)
        else if (copied < segment.text.length) result.push({...segment, text: segment.text.slice(copied), sourceMap: sliceSpeechSourceMap(segment.sourceMap, copied, segment.text.length)})
    }
    return result
}

const collectSpeechContent = (range, bookLanguages, selectedText, locateRange) => {
    if (range.collapsed) return []
    const candidates = speechReferenceAnchors(range).filter(anchor => visibleSpeechMath(anchor)
        && speechReferenceLabel(anchor, bookLanguages))
    const references = candidates.filter(anchor => !candidates.some(other => other !== anchor && other.contains(anchor)))
    const formulas = speechMathContainers(range).filter(formula => !references.some(anchor => anchor.contains(formula)))
    const atoms = [...references, ...formulas].sort((a, b) => a.compareDocumentPosition(b) & Node.DOCUMENT_POSITION_FOLLOWING ? -1 : 1)
    const options = { locateRange, sentences: true, deferSentences: true }
    const finish = segments => {
        // Empty spine documents can have a range ending outside <body>. They
        // have no speech source and must not be serialized as an EPUB CFI.
        if (!segments.length) return []
        // The source range distinguishes repeated language runs in one block.
        const sourceKey = locateRange(range)
        const parts = splitSpeechContent(coordinateSpeechSegments(segments, locateRange), locateRange, sourceKey)
        return serializeSpeechSegments(parts, locateRange, true)
    }
    if (!atoms.length) return finish(collectLanguageSegments(range, bookLanguages, selectedText, options))
    const result = []
    const cursor = range.cloneRange()
    const doc = range.startContainer.ownerDocument
    for (const atom of atoms) {
        const whole = rangeForNode(atom)
        if (whole.comparePoint(cursor.startContainer, cursor.startOffset) < 0) {
            const before = cursor.cloneRange()
            before.setEndBefore(atom)
            result.push(...collectLanguageSegments(before, bookLanguages, undefined, options))
        }
        if (references.includes(atom)) {
            const label = speechReferenceLabel(atom, bookLanguages)
            // Use original text-node boundaries: anchor wrappers are skipped
            // by the CFI filter, so element-only ranges can lose their extent.
            const walker = doc.createTreeWalker(atom, NodeFilter.SHOW_TEXT)
            const nodes = []
            while (walker.nextNode()) if (walker.currentNode.length) nodes.push(walker.currentNode)
            if (nodes.length) {
                const source = doc.createRange()
                source.setStart(nodes[0], 0)
                source.setEnd(nodes.at(-1), nodes.at(-1).length)
                const cfi = locateRange(source)
                const marks = Array.from(new Intl.Segmenter(label.lang || undefined, { granularity: 'word' }).segment(label.text))
                    .filter(word => word.isWordLike).map(word => ({ start: word.index, length: word.segment.length, cfi }))
                result.push({ type: 'reference', text: label.text, lang: label.lang, cfi, marks,
                    block: speechBlockFor(atom), inline: true,
                    sourceMap: [{ spokenStart: 0, spokenLength: label.text.length, cfi, exact: false }] })
            }
        } else {
            let text = speechMathText(atom)
            if (text && visibleSpeechMath(atom)) {
                // Anchor to the container, not its replaceable SVG/CHTML children.
                const source = doc.createRange()
                source.setStart(atom, 0)
                source.collapse(true)
                const cfi = locateRange(source)
                const lang = normalizeLanguageTag(preparedMathSpeech.get(atom)?.locale)
                    || normalizeLanguageTag(atom.getAttribute('data-semantic-locale'))
                    || (getLang(atom) ?? bookLanguageHint(bookLanguages))
                const block = speechBlockFor(atom)
                const inline = atom.getAttribute('display') !== 'true'
                const symbolCount = speechMathSymbolCount(atom)
                // A freshly generated SRE description can narrate longer formulas.
                // Keep the legacy limit when only a publisher's stored label exists.
                const tooComplex = !preparedMathSpeech.has(atom) && (symbolCount === null || symbolCount > 10)
                if (tooComplex) text = lang?.split('-')[0] === 'de'
                    ? 'zum Sprechen zu komplizierte Formel' : 'too complicated formula to speak'
                result.push({ type: 'math', text, lang, cfi, marks: [], block,
                    inline, symbolCount, tooComplex,
                    sourceMap: [{ spokenStart: 0, spokenLength: text.length, cfi, exact: false }] })
            } else if (visibleSpeechMath(atom)) {
                console.warn('MathJax formula has no prepared speech text')
            }
        }
        if (whole.comparePoint(range.endContainer, range.endOffset) <= 0) return finish(result)
        cursor.setStartAfter(atom)
    }
    result.push(...collectLanguageSegments(cursor, bookLanguages, undefined, options))
    return finish(result)
}

// Split before serializing the source ranges to CFIs: exact spans still point
// to individual text nodes, so sentence boundaries can be clipped in UTF-16.
const sliceSpeechSourceMap = (sourceMap, begin, end, firstSpan = 0) => {
    const spans = []
    for (let i = firstSpan; i < sourceMap.length && sourceMap[i].spokenStart < end; i++) {
        const span = sourceMap[i]
        const start = Math.max(begin, span.spokenStart)
        const stop = Math.min(end, span.spokenStart + span.spokenLength)
        if (stop <= start) continue
        spans.push({
            ...span,
            spokenStart: start - begin,
            spokenLength: stop - start,
            // Normalized whitespace has no one-to-one character mapping.
            // Retain its source range while clipping exact text precisely.
            start: span.exact ? span.start + start - span.spokenStart : span.start,
            end: span.exact ? span.start + stop - span.spokenStart : span.end,
        })
    }
    return spans
}

const splitSpeechSentences = ({ text, lang, sourceMap, block }) => {
    const sentences = new Intl.Segmenter(lang || undefined, { granularity: 'sentence' })
    const result = []
    let firstSpan = 0
    for (const { index, segment } of sentences.segment(text)) {
        const end = index + segment.length
        while (firstSpan < sourceMap.length
            && sourceMap[firstSpan].spokenStart + sourceMap[firstSpan].spokenLength <= index) firstSpan++
        result.push({ text: segment, lang, block, sourceMap: sliceSpeechSourceMap(sourceMap, index, end, firstSpan) })
    }
    return result
}

// Infer Sanskrit only for speech. Devanagari is also used by other languages;
// their explicit tags remain authoritative. Neutral punctuation/spacing stays
// with the preceding script, without changing any DOM text or UTF-16 offsets.
const speechLanguageRuns = (text, language = '', previousLanguage) => {
    const base = language.split('-')[0].toLowerCase()
    if (['sa', 'hi', 'mr', 'ne', 'mai', 'bho', 'awa', 'bra', 'doi', 'dgo', 'kok', 'new', 'pi', 'ks', 'sd'].includes(base))
        return [{ start: 0, end: text.length, lang: language }]
    const result = []
    let lang = previousLanguage, start = 0, offset = 0
    for (const character of text) {
        const next = /\p{Script=Devanagari}/u.test(character) ? 'sa'
            : /\p{Letter}/u.test(character) ? language : undefined
        if (next !== undefined) {
            if (lang !== undefined && lang !== next) {
                if (offset > start) result.push({ start, end: offset, lang })
                start = offset
            }
            lang = next
        }
        offset += character.length
    }
    result.push({ start, end: text.length, lang: lang ?? language })
    return result
}

// Translation keeps language runs; speech additionally requests sentence runs.
// All public offsets index the exact spoken string in UTF-16 code units.
const collectLanguageSegments = (range, bookLanguages, selectedText, { locateRange, sentences = false, deferSentences = false } = {}) => {
    const fallback = bookLanguageHint(bookLanguages)
    const doc = range.startContainer.ownerDocument
    const root = range.commonAncestorContainer
    let text = selectedText ?? range.toString()
    if (!text) return []
    const rawSegments = []
    const hidden = new WeakMap()
    const hiddenAncestor = el => {
        if (!el) return false
        if (hidden.has(el)) return hidden.get(el)
        const style = doc.defaultView?.getComputedStyle(el)
        const value = ['script', 'style', 'template', 'noscript'].includes(el.localName)
            || style?.display === 'none' || style?.contentVisibility === 'hidden'
            || hiddenAncestor(el.parentElement)
        hidden.set(el, value)
        return value
    }
    const visit = node => {
        if (!range.intersectsNode(node) || hiddenAncestor(node.parentElement)) return
        const visibility = node.parentElement && doc.defaultView?.getComputedStyle(node.parentElement).visibility
        if (visibility === 'hidden' || visibility === 'collapse') return
        const start = node === range.startContainer ? range.startOffset : 0
        const end = node === range.endContainer ? range.endOffset : node.length
        if (end > start) {
            const text = node.data.slice(start, end)
            const lang = getLang(node) ?? fallback
            const block = sentences ? speechBlockFor(node) : undefined
            const previous = rawSegments.at(-1)
            const runs = sentences ? speechLanguageRuns(text, lang, previous?.block === block ? previous.lang : undefined)
                : [{ start: 0, end: text.length, lang }]
            for (const run of runs) rawSegments.push({
                text: text.slice(run.start, run.end), lang: run.lang,
                node, start: start + run.start, end: start + run.end,
                ...(sentences ? { block } : {}),
            })
        }
    }
    let breakBefore = false
    if (root.nodeType === 3) visit(root)
    else {
        const walker = doc.createTreeWalker(root, NodeFilter.SHOW_TEXT | NodeFilter.SHOW_ELEMENT)
        while (walker.nextNode()) {
            const node = walker.currentNode
            if (node.nodeType === 1) {
                if (node.localName === 'br' && range.intersectsNode(node) && !hiddenAncestor(node)) breakBefore = true
                continue
            }
            const length = rawSegments.length
            visit(node)
            if (rawSegments.length > length) {
                rawSegments[length].breakBefore = breakBefore
                breakBefore = false
            }
        }
    }
    // Continuous reading has no live Selection. Build its input from visible
    // text nodes; Range.toString() would include display:none descendants.
    if (selectedText === undefined) text = rawSegments.map(part => (part.breakBefore ? '\n' : '') + part.text).join('')
    if (!text.trim()) return []

    // Selection.toString() reflects rendered whitespace, including paragraph
    // breaks and <br>. Project the DOM language runs onto that exact text so
    // markup indentation and hidden nodes do not change what gets translated.
    let position = 0
    const segments = []
    const append = (value, lang, sourceMap, block) => {
        if (!value) return
        // Split speech before sentence detection, including blocks without final
        // punctuation. Inline markup/language runs keep their paragraph identity.
        if (segments.at(-1)?.lang !== lang || (sentences && segments.at(-1)?.block !== block))
            segments.push({ text: '', lang, block, sourceMap: [] })
        const segment = segments.at(-1)
        const offset = segment.text.length
        segment.text += value
        for (const span of sourceMap)
            segment.sourceMap.push({ ...span, spokenStart: span.spokenStart + offset })
    }
    const unknownSelection = () => [{
        text,
        lang: rawSegments.length && rawSegments.every(part => part.lang === rawSegments[0].lang)
            ? rawSegments[0].lang : '',
        ...(locateRange ? {
            cfi: locateRange(range),
            sourceMap: [{ spokenStart: 0, spokenLength: text.length, cfi: locateRange(range), exact: false }],
        } : {}),
    }]
    for (const part of rawSegments) {
        const outputStart = position
        let sourceEnd = part.start
        const sourceMap = []
        const mapSpan = (start, end, outputBegin, outputEnd) => {
            if (!locateRange || outputEnd === outputBegin) return
            const exact = text.slice(outputBegin, outputEnd) === part.node.data.slice(start, end)
            const previous = sourceMap.at(-1)
            if (exact && previous?.exact && previous.end === start) {
                previous.end = end
                previous.spokenLength += outputEnd - outputBegin
            } else sourceMap.push({
                node: part.node, start, end, exact,
                spokenStart: outputBegin - outputStart, spokenLength: outputEnd - outputBegin,
            })
        }
        // Preserve normalization explicitly: exact text runs and collapsed or
        // inserted whitespace get separate source spans, including at node edges.
        for (const match of part.text.matchAll(/\S+/gu)) {
            const sourceStart = part.start + match.index
            const whitespaceStart = position
            while (position < text.length && /\s/u.test(text[position])) position++
            mapSpan(sourceEnd, sourceStart, whitespaceStart, position)
            if (!text.startsWith(match[0], position)) return unknownSelection()
            sourceEnd = sourceStart + match[0].length
            mapSpan(sourceStart, sourceEnd, position, position + match[0].length)
            position += match[0].length
        }
        if (/\s$/u.test(part.text)) {
            const whitespaceStart = position
            while (position < text.length && /\s/u.test(text[position])) position++
            mapSpan(sourceEnd, part.end, whitespaceStart, position)
        }
        append(text.slice(outputStart, position), part.lang, sourceMap, part.block)
    }
    const tail = text.slice(position)
    if (/\S/u.test(tail)) return unknownSelection()
    append(tail, segments.at(-1)?.lang ?? getLang(root) ?? fallback, locateRange ? [{
        node: range.endContainer, start: range.endOffset, end: range.endOffset,
        spokenStart: 0, spokenLength: tail.length, exact: false,
    }] : [], segments.at(-1)?.block ?? (sentences ? speechBlockFor(root) : undefined))
    if (deferSentences) return segments
    const output = sentences ? segments.flatMap(splitSpeechSentences) : segments
    return serializeSpeechSegments(output, locateRange, sentences)
}

const serializeSpeechSegments = (segments, locateRange, sentences) => {
    const sourceRange = (first, last = first) => {
        const source = first.node.ownerDocument.createRange()
        source.setStart(first.node, first.start)
        source.setEnd(last.node, last.end)
        return source
    }
    const wordMarks = (text, lang, sourceMap) => {
        const marks = []
        const words = new Intl.Segmenter(lang || undefined, { granularity: 'word' })
        let firstSpan = 0
        for (const { index, segment, isWordLike } of words.segment(text)) {
            if (!isWordLike) continue
            while (firstSpan < sourceMap.length
                && sourceMap[firstSpan].spokenStart + sourceMap[firstSpan].spokenLength <= index) firstSpan++
            const spans = sliceSpeechSourceMap(sourceMap, index, index + segment.length, firstSpan)
            // Do not guess word locations if DOM extraction was not exact.
            if (!spans.length || spans.some(span => !span.exact)) continue
            marks.push({ start: index, length: segment.length, cfi: locateRange(sourceRange(spans[0], spans.at(-1))) })
        }
        return marks
    }
    const blockIds = new WeakMap()
    const blockId = block => {
        if (!blockIds.has(block)) {
            blockIds.set(block, speechBlockId(block, locateRange))
        }
        return blockIds.get(block)
    }
    return segments.filter(segment => !sentences || segment.text.trim()).map(({ block, ...segment }) => {
        const { text, lang, sourceMap, ...properties } = segment
        const metadata = sentences && block && locateRange ? { blockId: blockId(block), blockTag: block.localName } : {}
        if (segment.cfi) return { ...segment, ...metadata }
        return {
            ...properties, text, lang, ...metadata,
            ...(locateRange ? {
                cfi: locateRange(sourceRange(sourceMap[0], sourceMap.at(-1))),
                sourceMap: sourceMap.map(span => ({
                    spokenStart: span.spokenStart, spokenLength: span.spokenLength,
                    cfi: locateRange(sourceRange(span)), exact: span.exact,
                })),
                ...(sentences ? { marks: wordMarks(text, lang, sourceMap) } : {}),
            } : {}),
        }
    })
}

const deriveSpeechWordMarks = (doc, sourceRange, spokenText, lang, locateRange) => {
    const textNodes = []
    const root = sourceRange.commonAncestorContainer
    const walker = doc.createTreeWalker(root, NodeFilter.SHOW_TEXT)
    const nodes = root.nodeType === Node.TEXT_NODE ? [root] : []
    let sourceText = ''
    while (walker.nextNode()) {
        nodes.push(walker.currentNode)
    }
    for (const node of nodes) {
        if (!sourceRange.intersectsNode(node)) continue
        const start = node === sourceRange.startContainer ? sourceRange.startOffset : 0
        const end = node === sourceRange.endContainer ? sourceRange.endOffset : node.length
        if (end <= start) continue
        textNodes.push({ node, start, end, offset: sourceText.length })
        sourceText += node.data.slice(start, end)
    }
    const spokenWords = Array.from(new Intl.Segmenter(lang || undefined, { granularity: 'word' }).segment(spokenText))
        .filter(word => word.isWordLike)
    const sourceWords = Array.from(new Intl.Segmenter(lang || undefined, { granularity: 'word' }).segment(sourceText))
        .filter(word => word.isWordLike)
    if (!spokenWords.length || spokenWords.length !== sourceWords.length
        || spokenWords.some((word, index) => word.segment !== sourceWords[index].segment)) return []
    const pointAt = offset => {
        const span = textNodes.find(item => offset >= item.offset && offset <= item.offset + item.end - item.start
            && (offset < item.offset + item.end - item.start || item === textNodes.at(-1)))
        if (!span) return null
        return { node: span.node, offset: span.start + offset - span.offset }
    }
    return spokenWords.flatMap((word, index) => {
        const sourceWord = sourceWords[index]
        const start = pointAt(sourceWord.index)
        const end = pointAt(sourceWord.index + sourceWord.segment.length)
        if (!start || !end) return []
        const range = doc.createRange()
        range.setStart(start.node, start.offset)
        range.setEnd(end.node, end.offset)
        return [{
            start: word.index, length: word.segment.length,
            cfi: locateRange(range),
        }]
    })
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

const requestJson = (method, url, body = null, timeout = 0, signal = null) => new Promise((resolve, reject) => {
    const request = new XMLHttpRequest()
    request.open(method, url)
    request.timeout = timeout
    const fail = (code, message) => reject(Object.assign(new Error(message), {code}))
    request.onerror = () => fail('network', 'BookServer connection failed or was blocked by the browser')
    request.ontimeout = () => fail('timeout', 'BookServer request timed out')
    request.onabort = () => fail('aborted', 'BookServer request cancelled')
    const abort = () => {
        request.abort()
        fail('aborted', 'BookServer request cancelled')
    }
    request.onloadend = () => signal?.removeEventListener('abort', abort)
    if (body !== null)
        request.setRequestHeader("Content-Type", "application/json")

    request.onreadystatechange = function () {
        // XHR reports DONE/status 0 before its more specific timeout/error/abort event.
        if (request.readyState !== 4 || request.status === 0)
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
            if (error.responseJson?.error === 'editorWorkingCopyExists')
                dispatch({ type: 'editor-working-copy-conflict', payload: error.responseJson.message })
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

    if (signal?.aborted) {
        fail('aborted', 'BookServer request cancelled')
        return
    }
    signal?.addEventListener('abort', abort, {once: true})
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

const crossReferenceAnchorSelector = ':is(a, span)[data-role="anchor"][data-anchor-type="crossref"][id]'
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
    // Transparent wrappers do not have their own CFI step. Use character
    // boundaries inside them, rather than offsets in a filtered element.
    const textBoundaries = range => {
        const walker = doc.createTreeWalker(range.commonAncestorContainer,
            NodeFilter.SHOW_TEXT | NodeFilter.SHOW_CDATA_SECTION)
        const nodes = []
        for (let node = walker.nextNode(); node; node = walker.nextNode())
            if (node.length && range.intersectsNode(node)) nodes.push(node)
        if (nodes.length) {
            range.setStart(nodes[0], 0)
            range.setEnd(nodes.at(-1), nodes.at(-1).length)
        }
        return range
    }
    for (const id of annotationAnchorElementIds(annotation)) {
        const wrapper = doc.getElementById(id)
        if (wrapper?.matches?.(annotationAnchorSelector)) {
            const range = doc.createRange()
            range.selectNodeContents(wrapper)
            return textBoundaries(range)
        }

        const begin = doc.getElementById(`${id}_begin`)
        const end = doc.getElementById(`${id}_end`)
        if (begin && end) {
            const range = doc.createRange()
            range.setStartAfter(begin)
            range.setEndBefore(end)
            return textBoundaries(range)
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

const drawReaderAnnotation = ({ draw, annotation, doc, range }) => {
    const { color } = annotation
    const options = { color, tooltip: annotation.note || "" }
    let renderer = Overlayer.highlight
    if (['underline', 'squiggly', 'strikethrough'].includes(color)) {
        const node = range.startContainer
        const el = node.nodeType === 1 ? node : node.parentElement
        options.writingMode = doc.defaultView.getComputedStyle(el).writingMode
        renderer = Overlayer[color]
    }
    draw(renderer, options)
}

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

const compactText = value => (value ?? "").replace(/\s+/g, " ").trim()

// The reader owns the captured selection/CFI; all form controls are native QML.
const createReference = (sourceBookId, cfi, text, options = {}) =>
    backend.openCrossReferenceEditor(sourceBookId, cfi, text, options)

const contextImageElement = target => target?.closest?.('img, image, svg') ?? null

const contextImageSource = element => {
    if (element?.localName === 'img')
        return element.getAttribute('data-arianna-original-src')
            || element.getAttribute('src') || element.currentSrc || element.src || ''
    const image = element?.localName === 'svg' ? element.querySelector('image') : element
    return image?.getAttribute('href') || image?.getAttributeNS('http://www.w3.org/1999/xlink', 'href') || ''
}

const markImageNotInverse = element => {
    element?.classList?.add('not_inverse')
    if (element?.localName === 'svg')
        for (const image of element.querySelectorAll('svg, image, img'))
            image.classList.add('not_inverse')
    for (let parent = element?.parentElement; parent; parent = parent.parentElement)
        if (parent.localName === 'svg')
            parent.classList.add('not_inverse')
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

globalThis.navigateWorkCoordinate = async text => {
    const current = globalThis.reader
    if (!current?.view || !current.bookId) return
    try {
        const result = await requestJson('POST', `${baseUrl}/${encodeURIComponent(current.bookId)}/workStructure/resolve`, {text, role: 'navigation'})
        if (globalThis.reader !== current || !current.view) return
        const locator = result.target?.locators?.[0]
        if (!result.resolved || !locator) {
            dispatch({type: 'work-coordinate-result', payload: {resolved: false}})
            return
        }
        await current.view.goTo(locator.cfi || locator.href)
        dispatch({type: 'work-coordinate-result', payload: {resolved: true}})
    } catch (error) { dispatch({type: 'work-coordinate-result', payload: {resolved: false}}) }
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
` + userStylesheet + '\n' + readerInteractionCSS]

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

const footnoteSource = document.getElementById('footnote-source')

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
    #position
    constructor(el, check, state = {}) {
        this.#el = el
        this.#check = check
        this.#state = state
        // A movement can reach both the reader and its chapter iframe. Each
        // clone must restore its own cursor, even at the same screen position.
        this.#position = { x: state.x, y: state.y }
        if (this.#state.hidden) this.hide()
        this.#el.addEventListener('mousemove', ({ screenX, screenY }) => {
            // check if it actually moved
            if (screenX === this.#position.x && screenY === this.#position.y) return
            this.#position.x = screenX, this.#position.y = screenY
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

            createReference(this.bookId, "", text, {
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
        const targetNavigationError = reference.targetNavigationError || ""
        const targetNavigationLocation = targetNavigationError ? "" : targetHrefLocation || targetLocation
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
            targetNavigationError,
            targetAnchorRepair: reference.targetAnchorRepair || null,
            targetEntry,
            sourceTitle,
        }
    }

    #openReference(context) {
        if (!context?.targetNavigationLocation) {
            console.warn('No target location available for reference');
            return false;
        }

        try {
            if (backend && typeof backend.openReferencePage === 'function') {
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

    #editReference(book, context) {
        createReference(this.bookId, "", context.text, {
            editMode: true, sourceAnchorId: context.ref, sourceAnchorTitle: context.sourceAnchorTitle,
            targetBookId: context.targetBookId, targetLocation: context.targetLocation,
            targetHrefLocation: context.targetHrefLocation, targetCfiLocation: context.targetCfiLocation,
            targetPreviewHtml: context.targetPreviewHtml,
        })
    }

    #deleteReference(context) {
        backend.showCrossReference(this.bookId, context, true)
    }

    async handle(book, event) {
        const context = await this.#loadReference(book, event.detail)
        if (!context) return
        if (!String(context.targetPreviewHtml ?? "").trim() && context.targetNavigationLocation) {
            this.#openReference(context)
            return
        }
        backend.showCrossReference(this.bookId, context, false)
    }

    async showContextMenu(book, detail, isCurrent = () => true) {
        const context = await this.#loadReference(book, detail, { createIfMissing: false })
        if (!context || !isCurrent())
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
    #footnoteView = null
    #footnoteDocument = null
    #footnoteRequest = ''
    #footnoteSerial = 0
    #footnoteHref = ''
    #footnoteHidden = false
    #footnoteLinks = []
    #bookReferenceHandler;
    #annotationKeyByValue = new Map()
    #annotationAnchorLocations
    #annotationNavigation = Promise.resolve()
    currentAnnotationKey = ""
    #contextImage = null
    #speechDocumentId = String(openRequest)
    #speechSelectionSerial = 0
    #speechSession = ''
    #speechHighlight = null
    #pendingSpeechMarks = new Map()
    #speechWordFallback = null
    #speechPaused = false
    #speechStream = null
    #speechSelectionRequest = 0
    #speechReferences = null
    #workStructure = null
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
            this.closeFootnote()
            if (!this.book || !this.view) {
                view.close()
                return
            }
            this.#footnoteView = view
            this.#footnoteSerial++
            view.addEventListener('link', e => {
                e.preventDefault()
                const { href } = e.detail
                this.view.goTo(href)
                this.closeFootnote()
            })
            view.addEventListener('external-link', e => {
                e.preventDefault()
                dispatch({ type: 'external-link', payload: e.detail })
            })
            footnoteSource.replaceChildren(view)

            const { renderer } = view
            renderer.setAttribute('flow', 'scrolled')
            renderer.setAttribute('margin', '12px')
            renderer.setAttribute('gap', '5%')
            renderer.setStyles(getCSS(this.style))
        })
        this.#footnoteHandler.addEventListener('render', async e => {
            const { view, href, hidden, type, doc, index } = e.detail
            const serial = this.#footnoteSerial
            await waitForMath(doc)
            if (view !== this.#footnoteView || serial !== this.#footnoteSerial || !this.book || !this.view) return
            const requestId = `${this.#speechDocumentId}:${serial}`
            const preview = createFootnotePreview(doc, this.book, index, requestId, globalThis.footnoteResourceOrigin, this.style)
            this.#footnoteDocument = doc
            this.#footnoteRequest = requestId
            this.#footnoteHref = href
            this.#footnoteHidden = hidden
            this.#footnoteLinks = preview.links
            const { uiText } = globalThis
            dispatch({ type: 'show-footnote', payload: {
                requestId, html: preview.html, baseUrl: document.baseURI, hidden,
                appearance: { requestId, ...preview.appearance },
                title: uiText.references[type] ?? uiText.references.footnote,
                goLabel: uiText.references[type + '-go'] ?? uiText.references['footnote-go'],
            } })
        })

    }
    closeFootnote(requestId) {
        if (requestId && requestId !== this.#footnoteRequest) return
        const oldRequest = this.#footnoteRequest
        this.#footnoteRequest = ''
        this.#footnoteDocument = null
        this.#footnoteSerial++
        this.#footnoteLinks = []
        this.#footnoteHref = ''
        this.#footnoteHidden = false
        this.#footnoteView?.close()
        this.#footnoteView?.remove()
        this.#footnoteView = null
        if (oldRequest) dispatch({ type: 'close-footnote', payload: { requestId: oldRequest } })
    }
    goToFootnote(requestId) {
        if (!requestId || requestId !== this.#footnoteRequest || this.#footnoteHidden) return
        const href = this.#footnoteHref
        this.closeFootnote(requestId)
        this.view?.goTo(href)
    }
    followFootnoteLink(requestId, index) {
        if (!requestId || requestId !== this.#footnoteRequest || !Number.isInteger(index)) return
        const link = this.#footnoteLinks[index]
        if (!link) return
        this.closeFootnote(requestId)
        if (link.external) dispatch({ type: 'external-link', payload: { href: link.href } })
        else this.view?.goTo(link.href)
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
        this.#loadWorkStructure()
    }
    close() {
        this.stopSpeechStream()
        this.clearSpeechHighlight()
        this.#pendingSpeechMarks.clear()
        this.closeFootnote()
        selectionAction?.()
        selectionAction = null
        this.view?.close()
        this.view?.remove()
        this.book?.destroy?.()
        this.view = null
        this.book = null
    }
    async prepareSpeech(doc, range, sectionIndex) {
        await Promise.all([prepareMathSpeech(doc, range), this.#prepareReferenceSpeech(doc, range), this.#prepareCoordinateSpeech(doc, sectionIndex)])
    }
    #loadWorkStructure() {
        if (!/^https?:\/\//.test(baseUrl) || !this.bookId) {
            this.#workStructure = Promise.resolve(null)
            return this.#workStructure
        }
        if (!this.#workStructure) {
            this.#workStructure = requestJson('GET', `${baseUrl}/${encodeURIComponent(this.bookId)}/workStructure`, null, 5000)
                .then(state => {
                    const record = state.validity === 'valid' ? state.record : null
                    if (this.view) dispatch({type: 'work-structure-ready', payload: {bookId: this.bookId, available: !!record}})
                    return record
                }).catch(() => null)
        }
        return this.#workStructure
    }
    async #prepareCoordinateSpeech(doc, sectionIndex) {
        this.#loadWorkStructure()
        const request = this.#workStructure
        const record = await request
        if (!record || !this.view || request !== this.#workStructure) return
        for (const node of record.nodes || []) {
            if (!node.marker || !/[.:-]/.test(node.marker)) continue
            const cfi = node.locators?.[0]?.cfi
            if (!cfi) continue
            try {
                const {index, anchor} = this.view.resolveCFI(cfi, cfiFilter)
                if (index !== sectionIndex) continue
                const target = anchor(doc)
                const element = target?.startContainer
                    ? (target.startContainer.nodeType === Node.ELEMENT_NODE ? target.startContainer : target.startContainer.parentElement) : target
                if (element?.ownerDocument === doc) preparedCoordinateSpeech.set(element, {marker: node.marker, semanticPath: semanticPath(record, node)})
            } catch (error) { console.warn('Unresolved structure speech target', error) }
        }
    }
    async #prepareReferenceSpeech(doc, range) {
        const selector = `${crossReferenceAnchorSelector}, ${legacyBookReferenceSelector}`
        const anchors = range ? speechReferenceAnchors(range) : Array.from(doc.querySelectorAll(selector))
        if (!anchors.length) return
        if (!this.#speechReferences) {
            // Bulk reads avoid a navigation/repair lookup for every spoken
            // anchor. These endpoints do not update references or book files.
            this.#speechReferences = Promise.all([
                requestJson('GET', `${baseUrl}/${encodeURIComponent(this.bookId)}/refs`),
                requestJson('GET', `${baseUrl}/books`),
            ]).then(([referenceData, bookData]) => {
                const books = new Map((Array.isArray(bookData) ? bookData : bookData?.books ?? [])
                    .map(book => [book.id, book.title]))
                const references = Array.isArray(referenceData) ? referenceData : referenceData?.refs ?? []
                return new Map(references.map(ref => [ref.sourceAnchorId,
                    { hint: ref.sourceAnchorTitle, title: books.get(ref.targetBookId) || '' }]))
            }).catch(error => {
                console.warn('Unable to load speech references; using anchor hints', error)
                return new Map()
            })
        }
        const metadata = this.#speechReferences
        const references = await metadata
        if (!this.view) return
        if (metadata !== this.#speechReferences) return this.#prepareReferenceSpeech(doc, range)
        for (const anchor of anchors)
            preparedReferenceSpeech.set(anchor, references.get(referenceIdFromElement(anchor)) || {})
    }
    invalidateSpeechReferences() {
        this.#speechReferences = null
        this.#workStructure = null
    }
    collectSpeechSegments(range, index, text) {
        if (!this.view || !this.book) return []
        const selectionId = ++this.#speechSelectionSerial
        const segments = collectSpeechContent(range, this.book?.metadata?.language, text,
            source => this.view.getCFI(index, source, cfiFilter))
        return segments.map((segment, i) => ({
            ...segment,
            id: `${this.#speechDocumentId}:${selectionId}:${i}`,
            documentId: this.#speechDocumentId,
            sectionIndex: index,
            href: this.book.sections[index]?.id ?? '',
        }))
    }
    async startSpeechStream(streamId, cfi, after = false, sessionId = '') {
        this.stopSpeechStream()
        if (!this.view || !cfi) return
        const { index, anchor } = this.view.resolveCFI(cfi, cfiFilter)
        const stream = { id: streamId, index, anchor, after, pending: [], done: false, loading: false }
        this.#speechStream = stream
        await this.#speechBatch(stream, sessionId, !sessionId, !!sessionId)
    }
    stopSpeechStream() {
        this.#speechStream = null
    }
    async nextSpeechBatch(streamId, sessionId, preload = false) {
        const stream = this.#speechStream
        if (stream?.id !== streamId || sessionId !== this.#speechSession) return
        await this.#speechBatch(stream, sessionId, false, preload)
    }
    #nextSpeechSection(index) {
        do { index++ } while (this.book.sections[index]?.linear === 'no')
        return index
    }
    async #speechBatch(stream, sessionId, initial, preload = false) {
        if (stream.loading) return
        stream.loading = true
        try {
            const segments = []
            let characters = 0
            // Finish the current DOM block even at the usual refill boundary,
            // so short paragraphs with many sentences still reach C++ together.
            while (characters < 8192 && (segments.length < 24
                || (stream.pending[0]?.blockId && stream.pending[0].blockId === segments.at(-1)?.blockId))) {
                if (this.#speechStream !== stream || !this.view) return
                if (stream.pending.length) {
                    const segment = stream.pending.shift()
                    segments.push(segment)
                    characters += segment.text.length
                    continue
                }
                if (stream.done) {
                    const next = this.#nextSpeechSection(stream.index)
                    if (preload || segments.length || next >= this.book.sections.length) break
                    stream.index = next
                    stream.iterator = null
                    stream.done = false
                }
                if (!stream.iterator) {
                    let content = this.view.renderer.getContents().find(item => item.index === stream.index)
                    if (!content) {
                        // Preloading must never navigate away from playing text.
                        if (preload) break
                        await this.view.renderer.goTo({ index: stream.index })
                        if (this.#speechStream !== stream || !this.view) return
                        content = this.view.renderer.getContents().find(item => item.index === stream.index)
                    }
                    if (!content?.doc?.body) throw new Error('Unable to load speech section')
                    await this.prepareSpeech(content.doc, undefined, content.index)
                    if (this.#speechStream !== stream || !this.view) return
                    stream.iterator = getBlocks(content.doc, { math: true })
                    if (stream.anchor) {
                        stream.start = stream.anchor(content.doc).cloneRange()
                        stream.start.collapse(!stream.after)
                        stream.anchor = null
                    }
                }
                const { value, done } = stream.iterator.next()
                stream.done = done
                if (done) continue
                const range = value.cloneRange()
                if (stream.start) {
                    const { startContainer, startOffset } = stream.start
                    const position = range.comparePoint(startContainer, startOffset)
                    if (position > 0) continue
                    if (position === 0) range.setStart(startContainer, startOffset)
                    stream.start = null
                }
                stream.pending = this.collectSpeechSegments(range, stream.index)
                    .filter(segment => segment.text.trim())
            }
            if (this.#speechStream !== stream) return
            dispatch({ type: 'speech-batch', payload: {
                streamId: stream.id, sessionId, initial, segments,
                hasMore: stream.pending.length > 0 || !stream.done
                    || this.#nextSpeechSection(stream.index) < this.book.sections.length,
            } })
        } catch (error) {
            if (this.#speechStream === stream) dispatch({ type: 'speech-batch', payload: {
                streamId: stream.id, sessionId, initial, segments: [], hasMore: false, error: String(error),
            } })
        } finally {
            stream.loading = false
        }
    }
    setSpeechSession(sessionId) {
        this.clearSpeechHighlight()
        this.#pendingSpeechMarks.clear()
        this.#speechSession = sessionId
        this.#speechPaused = false
    }
    setSpeechPaused(paused) {
        this.#speechPaused = paused
        const fallback = this.#speechWordFallback
        if (!fallback) return
        if (paused && fallback.timer !== null) {
            clearTimeout(fallback.timer)
            fallback.timer = null
            fallback.remaining = Math.max(0, fallback.deadline - performance.now())
        } else if (!paused && fallback.timer === null) {
            this.#scheduleSpeechWordFallback(fallback, fallback.remaining)
        }
    }
    showSpeechSegment(sessionId, segment, speechRate = 0) {
        if (sessionId !== this.#speechSession || segment.documentId !== this.#speechDocumentId) return
        this.clearSpeechHighlight()
        if (!segment.cfi) return
        const pendingMark = this.#pendingSpeechMarks.get(segment.id)
        this.#pendingSpeechMarks.clear()
        this.#speechHighlight = pendingMark
            ? { ...segment, cfi: pendingMark.cfi, wordStart: pendingMark.start, wordLength: pendingMark.length }
            : segment
        this.#drawSpeechHighlight()
        this.#followSpeechHighlight()
        if (!pendingMark && segment.type !== 'math') {
            const marks = this.#speechWordMarks(segment)
            if (marks.length) {
                this.#speechWordFallback = {
                    sessionId, segment, marks, index: 0, timer: null, deadline: 0,
                    remaining: 300, durationFactor: speechRate < 0 ? 1 - speechRate / 50 : 1 - speechRate / 150,
                }
                if (!this.#speechPaused) this.#scheduleSpeechWordFallback(this.#speechWordFallback, 300)
            }
        }
    }
    showSpeechMark(sessionId, segmentId, mark) {
        if (sessionId !== this.#speechSession || !mark.cfi) return
        if (segmentId !== this.#speechHighlight?.id) {
            if (this.#pendingSpeechMarks.size >= 32) this.#pendingSpeechMarks.delete(this.#pendingSpeechMarks.keys().next().value)
            this.#pendingSpeechMarks.set(segmentId, mark)
            return
        }
        if (this.#speechHighlight.type === 'math') return
        this.#clearSpeechWordFallback()
        this.#speechHighlight = { ...this.#speechHighlight, cfi: mark.cfi, wordStart: mark.start, wordLength: mark.length }
        this.#drawSpeechHighlight()
        this.#followSpeechHighlight()
    }
    #speechWordMarks(segment) {
        const existingMarks = segment.marks?.filter(mark => mark.cfi) ?? []
        if (existingMarks.length) return existingMarks
        const content = this.view?.renderer?.getContents?.().find(item => item.index === segment.sectionIndex)
        if (!content?.doc) return []
        try {
            const sourceRange = this.#speechHighlightTarget(content, segment)
            return deriveSpeechWordMarks(content.doc, sourceRange, segment.text, segment.lang,
                range => this.view.getCFI(content.index, range, cfiFilter))
        } catch (error) {
            console.warn('Unable to derive speech word locations', error)
            return []
        }
    }
    #scheduleSpeechWordFallback(fallback, delay) {
        fallback.remaining = delay
        fallback.deadline = performance.now() + delay
        fallback.timer = setTimeout(() => {
            fallback.timer = null
            if (fallback !== this.#speechWordFallback || fallback.sessionId !== this.#speechSession
                || fallback.segment.id !== this.#speechHighlight?.id || this.#speechPaused) return
            const mark = fallback.marks[fallback.index]
            if (!mark) {
                this.#clearSpeechWordFallback()
                return
            }
            this.#speechHighlight = { ...fallback.segment, cfi: mark.cfi, wordStart: mark.start, wordLength: mark.length }
            this.#drawSpeechHighlight()
            fallback.index++
            if (fallback.index >= fallback.marks.length) {
                this.#clearSpeechWordFallback()
                return
            }
            const next = fallback.marks[fallback.index]
            const word = fallback.segment.text.slice(mark.start, mark.start + mark.length)
            const gap = fallback.segment.text.slice(mark.start + mark.length, next.start)
            const syllables = word.toLocaleLowerCase().match(/[aeiouyäöü]+/gu)?.length || 1
            const pause = /[.!?]["'’”»›)\]]*\s*$/u.test(gap) ? 260
                : /[,;:]["'’”»›)\]]*\s*$/u.test(gap) ? 140 : 0
            const nextDelay = Math.max(100, Math.round((syllables * 150 + pause) * fallback.durationFactor))
            if (!this.#speechPaused) this.#scheduleSpeechWordFallback(fallback, nextDelay)
            else fallback.remaining = nextDelay
        }, delay)
    }
    #clearSpeechWordFallback() {
        const fallback = this.#speechWordFallback
        if (fallback && fallback.timer !== null) clearTimeout(fallback.timer)
        this.#speechWordFallback = null
    }
    async #followSpeechHighlight() {
        if (!this.#speechStream || !this.#speechHighlight || !this.view) return
        const segment = this.#speechHighlight
        const session = this.#speechSession
        const content = this.view.renderer.getContents().find(item => item.index === segment.sectionIndex)
        if (!content?.doc) return
        try {
            const range = this.#speechHighlightTarget(content, segment)
            // The location CFI is an estimate of the visible text. Use actual
            // layout bounds, including the second column and vertical/RTL flow.
            if (this.view.renderer.isRangeVisible?.(range)) return
            await this.view.renderer.scrollToAnchor?.(range)
            if (session === this.#speechSession && segment === this.#speechHighlight) this.#drawSpeechHighlight()
        } catch (error) {
            console.warn('Unable to follow speech position', error)
        }
    }
    clearSpeechHighlight(sessionId = this.#speechSession, segmentId) {
        if (sessionId !== this.#speechSession || (segmentId && segmentId !== this.#speechHighlight?.id)) return
        this.#clearSpeechWordFallback()
        for (const { overlayer } of this.view?.renderer?.getContents?.() ?? [])
            overlayer?.remove('arianna-speech')
        this.#speechHighlight = null
    }
    #speechHighlightTarget(content, segment) {
        const { index, anchor } = this.view.resolveCFI(segment.cfi, cfiFilter)
        if (index !== content.index) throw new Error('Speech section changed')
        const range = anchor(content.doc)
        if (segment.type !== 'math') return range
        const node = range.startContainer
        const formula = (node.nodeType === 1 ? node : node.parentElement)?.closest('mjx-container')
        if (!formula) throw new Error('Speech formula no longer exists')
        return formula.querySelector(':scope > svg, :scope > mjx-math') ?? formula
    }
    #drawSpeechHighlight() {
        const segment = this.#speechHighlight
        if (!segment || !this.view) return
        const content = this.view.renderer.getContents().find(item => item.index === segment.sectionIndex)
        if (!content?.overlayer || !content.doc) return
        try {
            const range = this.#speechHighlightTarget(content, segment)
            // Resolve again on redraw: MathJax can replace the rendered SVG.
            const target = segment.type === 'math'
                ? { getClientRects: () => {
                    try { return this.#speechHighlightTarget(content, segment).getClientRects() }
                    catch { return [] }
                } }
                : range
            // Use the existing renderer's redraw lifecycle, without taking over
            // the DOM selection or the click targets of personal annotations.
            content.overlayer.add('arianna-speech', target, rects => {
                const node = range.startContainer ?? range
                const el = node.nodeType === 1 ? node : node.parentElement
                const writingMode = content.doc.defaultView.getComputedStyle(el).writingMode
                const mark = Overlayer.underline(rects, {
                    color: this.style.searchResultColor || '#1a52b3', width: 2, writingMode,
                })
                mark.dataset.speechSegment = segment.id
                if (segment.type === 'math') mark.dataset.speechType = 'math'
                if (segment.wordStart !== undefined) {
                    mark.dataset.speechWordStart = segment.wordStart
                    mark.dataset.speechWordLength = segment.wordLength
                }
                return mark
            }, { interactive: false })
        } catch (error) {
            console.warn('Unable to resolve speech segment', error)
        }
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
        this.invalidateSpeechReferences()
        this.referenceList()
            .then(references =>
                dispatch({ type: 'reference-overview', payload: { bookId: this.bookId, references } }))
            .catch(error => {
                console.warn('Unable to build reference overview', error)
                dispatch({ type: 'reference-overview', payload: { bookId: this.bookId, references: [] } })
            })
    }
    async goToAnnotation(annotation) {
        const resolved = await this.#annotationForDisplay(annotation)
        if (!resolved || annotation?.invalid || !this.view) return
        this.currentAnnotationKey = annotationKey(annotation)
        this.#updateAnnotationRuntimeLocation(resolved)
        return this.view.goTo(resolved.value)
    }
    navigateAnnotation(annotations, previous) {
        // Serialize quick wheel ticks so every tick advances from the previous target.
        this.#annotationNavigation = this.#annotationNavigation.then(async () => {
            const notes = (await Promise.all(annotations.filter(annotation => annotation && !annotation.invalid)
                .map(annotation => this.#annotationForDisplay(annotation)))).filter(Boolean)
                .filter(annotation => isCFI.test(annotation.value))
                .sort((a, b) => compareCFI(a.value, b.value))
            if (!notes.length) return
            let index = notes.findIndex(annotation => annotationKey(annotation) === this.currentAnnotationKey)
            if (index >= 0) {
                index = (index + (previous ? -1 : 1) + notes.length) % notes.length
            } else {
                const current = this.view.lastLocation?.cfi
                index = previous ? notes.length - 1 : 0
                if (current) {
                    if (previous) {
                        for (let i = notes.length - 1; i >= 0; --i) {
                            if (compareCFI(notes[i].value, current) <= 0) { index = i; break }
                        }
                    } else {
                        const next = notes.findIndex(annotation => compareCFI(annotation.value, current) >= 0)
                        if (next >= 0) index = next
                    }
                }
            }
            await this.goToAnnotation(notes[index])
        }).catch(error => console.warn('Unable to navigate to remark', error))
        return this.#annotationNavigation
    }
    async #annotationForDisplay(annotation) {
        if (annotation?.anchorId) {
            if (!this.#annotationAnchorLocations) this.#annotationAnchorLocations = this.#indexAnnotationAnchors()
            const locations = await this.#annotationAnchorLocations
            const value = locations.get(compactAnnotationId(annotation.anchorId))
            if (value) return {...annotation, value, runtimeCfi: value}
            if (this.view) dispatch({type:'annotation-unresolved', payload:{
                bookId:this.bookId, annotationId:annotationKey(annotation), error:'MissingAnchor',
            }})
            return null
        }
        const value = annotation?.value || annotation?.cfiRange || annotation?.cfi || ''
        return isCFI.test(value) ? {...annotation, value, runtimeCfi:value} : null
    }
    async #indexAnnotationAnchors() {
        const result = new Map()
        const book = this.book
        for (const [index, section] of (book?.sections || []).entries()) {
            if (!this.view || this.book !== book) break
            try {
                const doc = await section.createDocument()
                if (!this.view || this.book !== book) break
                const counts = new Map()
                for (const el of doc.querySelectorAll('[id]')) counts.set(el.id, (counts.get(el.id) || 0) + 1)
                const ids = new Set(Array.from(doc.querySelectorAll('[data-role="anchor"][id][data-anchor-type^="annotation"]'),
                    el => el.id.replace(/_(begin|end)$/, '')))
                for (const id of ids) {
                    const key = compactAnnotationId(id)
                    const ambiguous = [id, `${id}_begin`, `${id}_end`].some(x => (counts.get(x) || 0) > 1)
                        || (counts.has(id) && (counts.has(`${id}_begin`) || counts.has(`${id}_end`)))
                    const range = ambiguous ? null : rangeForAnnotationAnchor(doc, {anchorId:id})
                    const value = range && !range.collapsed ? this.view.getCFI(index, range, cfiFilter) : null
                    result.set(key, result.has(key) ? null : value)
                }
            } catch (error) { console.warn('Cannot resolve remark anchors in section', index, error) }
        }
        return result
    }
    #drawAnnotation({ draw, annotation, doc, range }) {
        drawReaderAnnotation({ draw, annotation, doc, range })
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
                if (resolved && this.#drawAnchoredAnnotation(resolved)) {
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

        return setImageNotInverse(this.bookId, imageContext)
            .then(response => {
                if (response?.status !== 'updated' && response?.status !== 'unchanged')
                    throw new Error("BookServer did not confirm the image update")
                markImageNotInverse(imageContext.element)
                dispatchBookReloadNeeded("image-not-inverse", response)
            })
            .catch(error => {
                console.warn("Unable to set image not_inverse", error)
                dispatch({type: 'image-not-inverse-error', payload: error?.message ?? String(error)})
            })
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
        this.#footnoteView?.renderer?.setStyles?.(getCSS(this.style))
        if (this.#footnoteRequest && this.#footnoteDocument) {
            dispatch({ type: 'style-footnote', payload: {
                requestId: this.#footnoteRequest,
                ...createFootnoteAppearance(this.#footnoteDocument, this.style),
            } })
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
        this.view.addEventListener('create-overlay', e => {
            dispatch({ type: 'create-overlay', payload: e.detail })
            // The renderer attaches the new overlayer after this event returns.
            queueMicrotask(() => this.#drawSpeechHighlight())
        })
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
        doc.addEventListener('keydown', handlePageWidthKey, true)

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
            if (event.defaultPrevented) return
            const img = contextImageElement(event.target)
            if (!img) {
                this.#contextImage = null
                return
            }

            try {
                this.#contextImage = {
                    element: img,
                    cfi: this.view.getCFI(index, rangeForNode(img), cfiFilter),
                    src: contextImageSource(img),
                }
                // Inline SVGs need not be reported as image media by WebEngine.
                if (img.localName !== 'img') {
                    event.preventDefault()
                    event.stopPropagation()
                    dispatch({ type: 'show-image-context-menu' })
                }
            } catch (e) {
                this.#contextImage = null
                console.warn("Unable to create image CFI for context menu", e)
            }
        })

        let isSelecting = false
        doc.addEventListener('pointerdown', () => {
            isSelecting = true
            this.#speechSelectionRequest++
            dispatch({ type: 'speech-selection', payload: { segments: [] } })
        })
        doc.addEventListener('keydown', event => {
            if (['Escape', 'ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End'].includes(event.key)) {
                this.#speechSelectionRequest++
                dispatch({ type: 'speech-selection', payload: { segments: [] } })
            }
        })
        doc.addEventListener('selectionchange', debounce(async () => {
            const selection = doc.getSelection()
            const range = getSelectionRange(selection)
            // Menu dismissal clears the DOM selection. Keep its snapshot until
            // the next explicit document interaction, including keyboard selection.
            if (!range) return
            const request = this.#speechSelectionRequest
            const text = selection.toString()
            const snapshot = range.cloneRange()
            await this.prepareSpeech(doc, snapshot, index)
            const current = getSelectionRange(doc.getSelection())
            if (request !== this.#speechSelectionRequest || !this.view || !current
                || !this.view.renderer.getContents().some(item => item.doc === doc)
                || snapshot.compareBoundaryPoints(Range.START_TO_START, current) !== 0
                || snapshot.compareBoundaryPoints(Range.END_TO_END, current) !== 0) return
            dispatch({ type: 'speech-selection', payload: {
                segments: this.collectSpeechSegments(snapshot, index, text),
            } })
        }, 50))
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
        installReaderInteractions(doc, {
            referenceSelector: `${bookReferenceClickSelector}, a[href]`,
            annotations: event => {
                const overlayer = this.view?.renderer?.getContents?.().find(item => item.doc === doc)?.overlayer
                return (overlayer?.hitTestAll(event) ?? [])
                    .filter(([value]) => !value.startsWith('foliate-search:'))
                    .map(([value, range]) => ({
                        kind: 'annotation', text: range.toString(),
                        activate: () => this.#showAnnotation({ index, range, value,
                            annotationKey: this.#annotationKeyByValue.get(value) || value, pos: getPosition(range) }),
                        context: () => {
                            this.#contextImage = null
                            this.#showAnnotation({ index, range, value, contextMenu: true,
                                annotationKey: this.#annotationKeyByValue.get(value) || value, pos: getPosition(range) })
                        },
                    }))
            },
            reference: el => {
                if (!el.matches(bookReferenceClickSelector))
                    return {kind: 'reference', text: el.getAttribute('title') || el.textContent || '', nativeContext: true,
                        activate: () => this.view.followLink(el, index),
                        context: () => {
                            this.#contextImage = null
                            const href = el.getAttribute('href')
                            return globalThis.showSelection({type: 'reader-link',
                                href: this.book.sections[index]?.resolveHref?.(href) ?? href})
                                .then(action => { if (action === 'open-link') this.view?.followLink(el, index) })
                        }}
                const ref = referenceIdFromElement(el)
                if (!ref) return null
                const detail = { ref, text: el.textContent ?? '', title: el.getAttribute('title') ?? '',
                    role: el.dataset.role, anchorType: el.dataset.anchorType }
                return {kind: 'reference', text: detail.title || detail.text,
                    activate: () => this.view.dispatchEvent(new CustomEvent('book-reference', {detail})),
                    context: isCurrent => {
                        this.#contextImage = null
                        return this.#bookReferenceHandler.showContextMenu(this.book, detail, isCurrent)
                    },
                }
            },
            math: formula => {
                if (typeof doc.defaultView?.ariannaMathDetails !== 'function') return null
                const show = type => {
                    this.#contextImage = null
                    const details = doc.defaultView.ariannaMathDetails(formula)
                    if (!details?.mathml && !details?.tex) return
                    const source = doc.createRange()
                    source.setStart(formula, 0)
                    source.collapse(true)
                    details.speechSegment = { sectionIndex: index, cfi: this.view.getCFI(index, source, cfiFilter) }
                    dispatch({ type, payload: details })
                }
                return {kind: 'math', text: formula.getAttribute('aria-label') || '',
                    activate: () => show('show-math'), context: () => show('show-math-context-menu')}
            },
            choose: choices => globalThis.showSelection({ type: 'reader-targets', choices }),
            isActive: () => !!this.view?.renderer?.getContents?.().some(item => item.doc === doc),
        })
        this.#cursorAutohider.cloneFor(doc.documentElement)
    }
    #showAnnotation({ index, range, value, annotationKey, pos, contextMenu = false }) {
        globalThis.showSelection({ type: 'annotation', value, annotationKey: annotationKey || value, cfiRange: value, pos, contextMenu })
            .then(action => {
                if (action === 'select')
                    this.#showSelection({ index, range, value, pos })
                else if (action === 'open-annotation')
                    this.#showAnnotation({ index, range, value, annotationKey, pos })
            })
    }
    async #showSelection({ index, range, lang, value, pos, text }) {
        if (!text) {
            const sel = range.startContainer.ownerDocument.getSelection()
            sel.removeAllRanges()
            sel.addRange(range)
            text = sel.toString()
        }
        const doc = range.startContainer.ownerDocument
        const request = this.#speechSelectionRequest
        await this.prepareSpeech(doc, range, index)
        if (request !== this.#speechSelectionRequest || !this.view
            || !this.view.renderer.getContents().some(item => item.doc === doc)) return
        const content = range.toString()
        // Snapshot both forms before opening the menu. Translation should retain
        // sentence context within each language instead of issuing one request per sentence.
        const segments = this.collectSpeechSegments(range, index, text)
        const translationSegments = collectLanguageSegments(range, this.book?.metadata?.language, text)
        dispatch({ type: 'speech-selection', payload: { segments } })
        lang = getLang(range.commonAncestorContainer) ?? bookLanguageHint(this.book?.metadata?.language)
        const selectionProgress = this.view.getProgressOf(index, range)
        const readerContext = collectReaderSelectionContext(range, {
            bookLanguage: bookLanguageHint(this.book?.metadata?.language),
        })
        const speechStart = range.cloneRange()
        if (speechStart.startContainer.nodeType === 1) {
            const child = speechStart.startContainer.childNodes[speechStart.startOffset]
            if (child?.nodeType === 1) speechStart.setStart(child, 0)
        }
        speechStart.collapse(true)
        const speechCfi = this.view.getCFI(index, speechStart, cfiFilter)
        globalThis.showSelection({
            type: 'selection',
            text,
            content,
            lang,
            value,
            pos,
            ...selectionProgress,
            bookId: this.bookId,
            sectionIndex: index,
            readerContext,
        }).then(action => {
            switch (action) {
                case 'reference':
                    createReference(this.bookId, value, text);
                    break;
                case 'copy': getHTML(range).then(html =>
                    dispatch({ type: 'selection', payload: { action, text, html } }))
                    break
                case 'find':
                    dispatch({ type: 'selection-find', payload: { text } })
                    break
                case 'translate':
                    dispatch({ type: 'selection', payload: { action, text, pos, segments: translationSegments } })
                    break
                case 'read-aloud':
                    dispatch({ type: 'selection', payload: { action, text, pos, segments } })
                    break
                case 'read-document':
                    dispatch({ type: 'selection', payload: { action, cfi: speechCfi } })
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
        const { targetCfi = '', ...searchOptions } = options;

        for await (const result of this.view.search({ query, ...searchOptions })) {
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

        // Compare structural CFIs: package assertions can differ between the library and the reader.
        const selectedResult = targetCfi && isCFI.test(targetCfi)
            ? results.findIndex(result => compareCFI(result.cfi, targetCfi) === 0) : -1;
        dispatch({ type: 'find-results', payload: { query, results, targetCfi, selectedResult } });
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

globalThis.init = ({ uiText, resourceOrigin = '' }) => {
    globalThis.uiText = uiText
    globalThis.footnoteResourceOrigin = resourceOrigin

    format.loc = (a, b) => printf(uiText.loc, [a, b])
    format.page = (a, b) => b
        ? printf(uiText.page, [a, b])
        : printf(uiText.pageWithoutTotal, [a])

}
