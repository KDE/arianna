// SPDX-License-Identifier: MIT
// Shared by both EPUB renderers. The frame must be visible while MathJax
// computes em sizes, but no pagination/relocate event may precede this barrier.
export const waitForMath = async doc => {
    const script = doc.querySelector('script[data-arianna-mathjax]')
    const win = doc.defaultView
    if (!script && !win?.MathJax?.startup?.promise) return
    let timer
    try {
        if (script) {
            if (!win.ariannaMathReady) {
                await Promise.race([
                    new Promise(resolve => {
                        if (script.dataset.ariannaMathStatus === 'failed') return resolve()
                        script.addEventListener('load', resolve, { once: true })
                        script.addEventListener('error', resolve, { once: true })
                    }),
                    new Promise(resolve => { timer = setTimeout(resolve, 20000) }),
                ])
                clearTimeout(timer)
            }
            // Reader initialization has bounded individual operations. Do not
            // finalize a large chapter while conversions are still running.
            await win.ariannaMathReady
            await doc.fonts.ready
            return
        }
        await Promise.race([
            (async () => {
                // Also wait for an EPUB-owned compatible installation.
                await win.MathJax?.startup?.promise
                await win.MathJax?.whenReady?.(() => {})
                await doc.fonts.ready
            })(),
            new Promise(resolve => { timer = setTimeout(resolve, 20000) }),
        ])
    } catch (error) { console.warn('EPUB math layout failed', error) }
    finally { clearTimeout(timer) }
}

// Reader wrappers are transparent to CFI indexing; generated SVG is absent.
export const mathCFIFilter = node => node.nodeType === 1
    ? node.hasAttribute('data-arianna-math-output') ? NodeFilter.FILTER_REJECT
        : node.hasAttribute('data-arianna-math-wrapper') || (node.localName === 'span' && node.hasAttribute('data-arianna-math-source'))
            ? NodeFilter.FILTER_SKIP : NodeFilter.FILTER_ACCEPT
    : NodeFilter.FILTER_ACCEPT

// Selections inside generated math map to the source expression. This also
// gives speech segments a stable formula CFI, independent of SVG children.
export const mathSourceBoundary = (node, offset, end = false) => {
    const el = node.nodeType === 1 ? node : node.parentElement
    const output = el?.closest('[data-arianna-math-output]')
    if (!output) return { node, offset }
    const source = output.parentElement.querySelector(':scope > [data-arianna-math-source]')
    if (!source) return { node, offset }
    if (source.localName === 'span') {
        const text = source.firstChild
        return { node: text, offset: end ? text.length : 0 }
    }
    return { node: source, offset: end ? source.childNodes.length : 0 }
}

export const mathVisualBoundary = (node, offset, end = false) => {
    const el = node.nodeType === 1 ? node : node.parentElement
    const source = el?.closest('[data-arianna-math-source]')
    const output = source?.parentElement.querySelector(':scope > [data-arianna-math-output]')
    return output ? { node: output, offset: end ? output.childNodes.length : 0 } : { node, offset }
}

export const mathVisualTarget = target => {
    const node = target?.nodeType ? target : target?.commonAncestorContainer
    const visual = node ? mathVisualBoundary(node, 0).node : node
    return visual !== node ? visual : target
}
