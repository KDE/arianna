// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Cursor artwork and hit routing are transient reader UI. Neither changes the
// chapter's text nodes, anchors, geometry or CFI coordinate system.
const kinds = ['annotation', 'reference', 'math']
const glyphs = {
    annotation: '<path d="M1 1h16v11H9l-5 4v-4H1Z"/><path d="M4 4h10M4 7h7"/>',
    reference: '<path d="m7 5 2-2a4 4 0 0 1 6 6l-3 3a4 4 0 0 1-5 0M10 11l-2 2a4 4 0 0 1-6-6l3-3a4 4 0 0 1 5 0"/>',
    math: '<path d="m0 9 3-2 3 8L11 1h7M12 7l6 7m0-7-6 7"/>',
}
// Paint the badges in the reader shell instead of using a CSS cursor image.
// Chromium's computed cursor and even Qt's internal bitmap cursor do not prove
// that the desktop actually displays that image. The shell also keeps the
// badges outside the EPUB's DOM, author CSS and image-src policy.
const badges = new WeakMap()
const cursorBadge = doc => {
    const frame = doc.defaultView?.frameElement
    const host = frame?.ownerDocument ?? doc
    let state = badges.get(host)
    const clear = () => {
        state = badges.get(host)
        if (state?.owner === doc) {
            state.element.style.display = 'none'
            state.owner = null
        }
    }
    return {
        clear,
        show(types, event) {
            if (!host.body || !host.defaultView) return
            state = badges.get(host)
            if (!state) {
                const element = host.createElementNS('http://www.w3.org/2000/svg', 'svg')
                element.setAttribute('aria-hidden', 'true')
                element.style.cssText = 'position:fixed;display:none;pointer-events:none;z-index:2147483647;user-select:none;color:var(--fg,CanvasText);'
                host.body.append(element)
                state = {element}
                badges.set(host, state)
            }
            const {element} = state
            const value = types.join(' ')
            const width = 16 * types.length + 4
            if (element.getAttribute('data-arianna-reader-cursor-badge') !== value) {
                element.setAttribute('data-arianna-reader-cursor-badge', value)
                element.setAttribute('width', width)
                element.setAttribute('height', 20)
                // Reader colors update in place, including System/Inverse mode.
                element.innerHTML = `<rect x=".5" y=".5" width="${width - 1}" height="19" rx="3" fill="var(--bg,Canvas)" stroke="currentColor" stroke-opacity=".65"/>`
                    + types.map((kind, i) => `<g transform="translate(${3 + 16 * i} 3) scale(.8)" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">${glyphs[kind]}</g>`).join('')
            }
            const rect = frame?.getBoundingClientRect()
            const x = rect ? rect.left + event.clientX * rect.width / frame.clientWidth : event.clientX
            const y = rect ? rect.top + event.clientY * rect.height / frame.clientHeight : event.clientY
            const viewport = host.defaultView
            element.style.left = `${Math.max(0, Math.min(x + 6, viewport.innerWidth - width))}px`
            element.style.top = `${Math.max(0, Math.min(y + 16, viewport.innerHeight - 20))}px`
            element.style.display = 'block'
            state.owner = doc
        },
    }
}

export const readerInteractionCSS = Array.from({length: 7}, (_, i) => {
    const types = kinds.filter((_, bit) => (i + 1) & (1 << bit))
    // Let the existing idle-cursor hider take precedence.
    const root = `html[data-arianna-reader-cursor="${types.join(' ')}"]:not([style*="cursor: none"])`
    return `${root}, ${root} * { cursor: pointer !important; }`
}).join('\n')

export const installReaderInteractions = (doc, options) => {
    let generation = 0
    const root = doc.documentElement
    const badge = cursorBadge(doc)
    const clearCursor = () => {
        root.removeAttribute('data-arianna-reader-cursor')
        badge.clear()
    }
    const targetsAt = event => {
        const targets = options.annotations?.(event) ?? []
        const target = event.target?.nodeType === 1 ? event.target : event.target?.parentElement
        for (let el = target?.closest?.(options.referenceSelector); el;
            el = el.parentElement?.closest?.(options.referenceSelector)) {
            const reference = options.reference(el)
            if (reference) targets.push(reference)
        }
        const formula = target?.closest?.('mjx-container')
        if (formula && !target.closest('a[href],a[data-mjx-href]')) {
            const math = options.math(formula)
            if (math) targets.push(math)
        }
        return targets
    }
    const hasSelection = () => doc.getSelection()?.type === 'Range'
    const hover = event => {
        if (event.buttons || hasSelection() || root.style.cursor === 'none') return clearCursor()
        const found = new Set(targetsAt(event).map(target => target.kind))
        const types = kinds.filter(kind => found.has(kind))
        const value = types.join(' ')
        if (!value) clearCursor()
        else {
            if (root.getAttribute('data-arianna-reader-cursor') !== value)
                root.setAttribute('data-arianna-reader-cursor', value)
            badge.show(types, event)
        }
    }
    // Mouse events also cover WebEngine/desktop paths without Pointer Events.
    for (const type of ['pointermove', 'mouseover'])
        doc.addEventListener(type, hover, {passive: true, capture: true})
    // The root's idle hider restores the cursor during mousemove. Refresh after
    // that listener, including the first movement after it has hidden the cursor.
    doc.addEventListener('mousemove', hover, {passive: true})
    doc.addEventListener('pointerleave', clearCursor)
    doc.addEventListener('mouseleave', clearCursor)
    const pointerDown = () => { generation++; clearCursor() }
    doc.addEventListener('pointerdown', pointerDown, {passive: true})
    doc.addEventListener('mousedown', pointerDown, {passive: true})
    doc.addEventListener('scroll', clearCursor, {passive: true, capture: true})
    doc.addEventListener('selectionchange', () => {
        if (hasSelection()) clearCursor()
    })
    doc.defaultView?.addEventListener('blur', clearCursor)
    doc.defaultView?.addEventListener('pagehide', clearCursor)
    const idle = new MutationObserver(() => {
        if (root.style.cursor === 'none') clearCursor()
    })
    idle.observe(root, {attributes: true, attributeFilter: ['style']})
    doc.defaultView?.addEventListener('pagehide', () => idle.disconnect(), {once: true})
    const openTarget = async (event, contextMenu) => {
        if (event.defaultPrevented || (!contextMenu && event.button !== 0) || event.ctrlKey || event.altKey
            || event.metaKey || event.shiftKey) return
        const targets = targetsAt(event)
        if (!targets.length) return
        // Formula context actions preserve an existing text selection, as the
        // original MathJax handler did. Other selected text keeps its native menu.
        if (hasSelection() && !(contextMenu && targets.some(target => target.kind === 'math'))) return
        // Plain EPUB links retain WebEngine's menu. If a saved remark overlaps
        // one, the chooser also makes that link's open/copy actions reachable.
        if (contextMenu && targets.every(target => target.nativeContext)) return
        // Capture before Foliate's link/overlay listeners; those respect this
        // cancellation, so a single click can open only the chosen action.
        event.preventDefault()
        const request = ++generation
        const isCurrent = () => request === generation && options.isActive()
        try {
            const index = targets.length === 1 ? 0 : await options.choose(targets.map(({kind, text}) => ({kind, text})))
            if (!isCurrent() || !Number.isInteger(index)) return
            await targets[index]?.[contextMenu ? 'context' : 'activate']?.(isCurrent)
        } catch (error) {
            console.warn('Unable to open reader target', error)
        }
    }
    doc.addEventListener('click', event => openTarget(event, false), true)
    doc.addEventListener('contextmenu', event => openTarget(event, true), true)
}
