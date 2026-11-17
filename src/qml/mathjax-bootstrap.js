// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// This file is reader code. Never interpret text/x-mathjax-config, copy author
// configuration, or evaluate code found in a book.
(() => {
    'use strict'
    const bootstrap = document.currentScript
    const base = new URL('.', bootstrap.src).href
    const XHTML = 'http://www.w3.org/1999/xhtml'
    const create = name => document.createElementNS(XHTML, name)
    const head = document.head ?? document.documentElement
    const domReady = document.readyState === 'loading'
        ? new Promise(resolve => document.addEventListener('DOMContentLoaded', resolve, { once: true }))
        : Promise.resolve()
    const load = src => new Promise((resolve, reject) => {
        const script = create('script')
        script.src = src
        script.onload = resolve
        script.onerror = () => reject(new Error(`Cannot load local MathJax: ${src}`))
        head.append(script)
    })
    const deadline = promise => {
        let timer
        return Promise.race([promise, new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error('MathJax initialization timed out')), 15000)
        })]).finally(() => clearTimeout(timer))
    }
    const compatible = () => /^[4-9]\./.test(window.MathJax?.version ?? '')
        && typeof window.MathJax?.startup?.promise?.then === 'function'
    const authorLoader = Array.from(document.querySelectorAll('script[src]')).find(script =>
        script !== bootstrap && (script.hasAttribute('data-arianna-mathjax-author-loader') || script.id === 'MathJax-script'
            || /(?:mathjax|tex-(?:mml-)?(?:svg|chtml)|mml-(?:svg|chtml))/i.test(script.getAttribute('src'))))
    const formulaSources = new WeakMap()
    // Return data for an isolated formula window, never author configuration.
    window.ariannaMathDetails = formula => {
        const mathjax = window.MathJax
        const doc = mathjax?.startup?.document
        const source = formulaSources.get(formula)
        const item = Array.from(doc?.math ?? []).find(item => item.typesetRoot === formula)
        const mathml = source?.mathml || (item && doc.menu?.toMML(item)) || ''
        const tex = source?.tex ?? (item?.inputJax?.name === 'TeX' ? item.math : '')
        return { mathml, tex, speech: formula.getAttribute('data-semantic-speech') || '',
            locale: formula.getAttribute('data-semantic-locale') || 'en',
            display: formula.getAttribute('display') === 'true' }
    }
    const speechVariants = new WeakMap()
    window.ariannaMathSpeech = async (formula, options = {}) => {
        await window.ariannaMathReady
        const doc = window.MathJax?.startup?.document
        const details = window.ariannaMathDetails(formula)
        const locale = (options.locale || details.locale).replace('_', '-').split('-')[0].toLowerCase()
        if (!details.mathml || !doc?.webworker?.speechFor
            || !['en', 'de', 'fr', 'es', 'it', 'hi', 'ko', 'nb', 'nn', 'sv'].includes(locale)) return null
        const style = options.concise === false ? 'default' : 'Fraction_Over:Ellipses_AndSoOn'
        const key = `${locale}:${style}`
        let variants = speechVariants.get(formula)
        if (!variants) speechVariants.set(formula, variants = new Map())
        if (!variants.has(key)) {
            // speechFor returns data without attaching it to the original SVG.
            // A separate MathItem avoids the transient conversion item reused
            // by tex2svgPromise(), and never compiles/renders the source again.
            const request = deadline((async () => {
                const item = new doc.options.MathItem('', doc.inputJax[0], details.display)
                const mathml = item.toEnriched(details.mathml)
                const data = await doc.webworker.speechFor(mathml, {
                    ...doc.options.sre, locale, domain: 'clearspeak', style,
                    enableSpeech: true, enableBraille: false, modality: 'speech',
                }, item)
                return data.label?.trim() ? { text: data.label.trim(), locale: data.options.locale, mathml: details.mathml } : null
            })()).catch(error => { variants.delete(key); throw error })
            variants.set(key, request)
        }
        return variants.get(key)
    }
    const disableMathInteractions = mathjax => {
        const doc = mathjax.startup.document
        if (Object.hasOwn(doc.options, 'enableMenu')) doc.options.enableMenu = false
        if (Object.hasOwn(doc.options, 'enableExplorerHelp')) doc.options.enableExplorerHelp = false
        // v4's menu constructor enables the explorer again and copies its
        // settings into a11y, even with enableMenu/enableExplorer=false.
        // Disable interactive tools on the live document, retaining SRE labels.
        if (doc.options.a11y) {
            for (const key of ['speech', 'braille', 'keyMagnifier', 'mouseMagnifier',
                'hover', 'infoPrefix', 'infoRole', 'infoType', 'flame', 'treeColoring',
                'subtitles', 'viewBraille', 'voicing']) {
                // EPUB installations can omit the explorer. Adding options
                // then produces "Invalid option" on document reconstruction.
                if (Object.hasOwn(doc.options.a11y, key)) doc.options.a11y[key] = false
            }
        }
        if (doc.menu) {
            // Existing EPUB output already has listeners, including zoom
            // listeners driven by settings persisted in localStorage.
            doc.menu.settings.zoom = 'NoZoom'
            doc.menu.clear()
        }
        for (const item of doc.math) {
            for (const explorer of Object.values(item.explorers?.explorers ?? {})) {
                explorer.Stop()
                // Detach() also removes accessible output. Remove only the
                // handlers so the SVG DOM, speech and saved CFIs are retained.
                explorer.RemoveEvents()
                explorer.restoreAnchors?.()
            }
        }
    }
    const initialize = async () => {
        await domReady
        await deadline(load(`${base}mathjax-options.js`))
        const parameters = new URL(bootstrap.src).searchParams
        let rendering = null
        if (parameters.has('rendering')) {
            try { rendering = ariannaMathDisplay.normalize(JSON.parse(parameters.get('rendering'))) }
            catch { rendering = ariannaMathDisplay.normalize() }
        }
        const language = (document.documentElement.getAttribute('xml:lang')
            || document.documentElement.lang || document.body?.getAttribute('xml:lang')
            || document.body?.lang || 'en').split('-')[0].toLowerCase()
        const locale = ['en', 'de', 'fr', 'es', 'it', 'hi', 'ko', 'nb', 'nn', 'sv'].includes(language) ? language : 'en'
        let sources = []
        let mathjax
        let originalCache
        // A book's own installation retains its own security policy and config.
        // The reader never loads another installation alongside it.
        if (authorLoader || compatible()) {
            await deadline(new Promise((resolve, reject) => {
                if (compatible()) return resolve()
                authorLoader?.addEventListener('load', resolve, { once: true })
                authorLoader?.addEventListener('error', reject, { once: true })
                // The loader can have finished before this deferred bootstrap.
                if (window.MathJax?.startup?.promise) resolve()
            }))
            if (!compatible()) throw new Error('The EPUB MathJax installation is incompatible with MathJax 4')
            await deadline(window.MathJax.startup.promise)
            await deadline(window.MathJax.whenReady?.(() => {}))
            disableMathInteractions(window.MathJax)
            if (!rendering) return // Keep the existing rendered-DOM CFI baseline.
            mathjax = window.MathJax
            originalCache = document.getElementById('MJX-SVG-global-cache')
            // Retain the author's original rendered tree as the CFI source.
            // Only the reader's new output is excluded from CFI indexing.
            sources = Array.from(mathjax.startup.document.math).map(item => ({
                node: item.typesetRoot, mathml: mathjax.startup.document.menu.toMML(item),
                tex: item.inputJax.name === 'TeX' ? item.math : '', display: item.display,
                locale: item.typesetRoot.getAttribute('data-semantic-locale') || locale,
            })).filter(source => source.node?.isConnected)
            const doc = mathjax.startup.document
            if (!doc.inputJax.some(input => input.name === 'MathML')) {
                // tex-svg.js has no MathML input. Attach the local parser to
                // the same document without re-running author startup code.
                await deadline(load(`${base}input/mml.js`))
                const input = new mathjax.startup.constructors.mml()
                input.setAdaptor(doc.adaptor)
                input.setMmlFactory(doc.mmlFactory)
                input.initialize()
                doc.inputJax.push(input)
            }
        } else {
            // Replace any inert author config object rather than merging callbacks,
            // loader paths or executable macros into reader-owned configuration.
            window.MathJax = {
                loader: { paths: { mathjax: base.replace(/\/$/, ''), fonts: `${base}fonts`, sre: `${base}sre`, mathmaps: `${base}sre/mathmaps` }, load: ['ui/safe'] },
                startup: { typeset: false },
                tex: { packages: ['base', 'ams', 'newcommand', 'noundefined', 'configmacros', 'textmacros'],
                    maxBuffer: 10000, maxMacros: 1000 },
                output: { font: 'mathjax-newcm', fontPath: `${base}fonts/%%FONT%%-font` },
                svg: { fontCache: 'local' },
                options: {
                    enableMenu: false, enableExplorer: false, enableSpeech: true, enableBraille: false,
                    sre: { locale },
                    worker: { path: `${base}sre`, maps: `${base}sre/mathmaps` },
                    safeOptions: { allow: { URLs: 'none', classes: 'none', cssIDs: 'none', styles: 'none' } },
                },
            }
            bootstrap.setAttribute('data-arianna-math-phase', 'load')
            await deadline(load(`${base}tex-mml-svg.js`))
            mathjax = window.MathJax
            bootstrap.setAttribute('data-arianna-math-phase', 'startup')
            await deadline(mathjax.startup.promise)
            disableMathInteractions(mathjax)
            const marked = !!document.querySelector('script[type="text/x-mathjax-config"]')
            const ignored = 'script,style,pre,code,textarea,mjx-container,[data-arianna-math-source],[data-arianna-math-output]'
            sources = Array.from(document.querySelectorAll('math')).filter(node =>
                !node.parentElement?.closest(`math,${ignored}`))
                .map(node => ({ node, mml: true, display: node.getAttribute('display') === 'block' }))
            const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, {
                acceptNode: node => node.parentElement?.closest(`math,${ignored}`)
                    ? NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT,
            })
            // Single-dollar math must have math-like content unless Calibre opts in.
            // Whitespace next to dollar delimiters excludes ordinary currency pairs.
            const pattern = /\\\(([\s\S]+?)\\\)|\\\[([\s\S]+?)\\\]|\$\$([\s\S]+?)\$\$|(?<![\\$])\$(?!\s)([^$\n]+?)(?<!\s)\$(?!\$)/g
            for (let node = walker.nextNode(); node; node = walker.nextNode()) {
                for (const match of node.data.matchAll(pattern)) {
                    if (match[4] && !marked && !/[A-Za-z\\^_=+*/{}]/.test(match[4])) continue
                    // Do not treat an escaped opening delimiter as mathematics.
                    let escapes = 0
                    for (let i = match.index - 1; i >= 0 && node.data[i] === '\\'; --i) ++escapes
                    if (escapes % 2) continue
                    sources.push({ node, start: match.index, length: match[0].length,
                        tex: match[1] ?? match[2] ?? match[3] ?? match[4], display: !!(match[2] ?? match[3]) })
                }
            }
            for (const node of document.querySelectorAll('script[type^="math/tex"]')) {
                sources.push({ node, tex: node.textContent, display: /mode\s*=\s*display/.test(node.type) })
            }
        }
        if (rendering) ariannaMathDisplay.configure(mathjax, rendering)
        const convert = async (source, width) => {
            if (rendering) ariannaMathDisplay.configure(mathjax, rendering, source.display)
            const metrics = rendering ? mathjax.getMetricsFor(source.context, source.display) : {}
            if (width > 0) metrics.containerWidth = Math.min(width, source.context.getBoundingClientRect().width || width)
            const options = {...metrics, display: source.display}
            return deadline(source.mathml || source.mml
                ? mathjax.startup.document.convertPromise(source.mathml || new XMLSerializer().serializeToString(source.node),
                    {...options, format: 'MathML'})
                : mathjax.tex2svgPromise(source.tex, options))
        }
        const retain = (output, source) => {
            output.setAttribute('data-arianna-math-output', '')
            output.setAttribute('data-semantic-locale', source.locale || locale)
            formulaSources.set(output, {mathml: source.mathml, tex: source.tex ?? ''})
        }
        const attributes = (target, replacement) => {
            for (const attr of Array.from(target.attributes))
                if (!replacement.hasAttributeNS(attr.namespaceURI, attr.localName))
                    target.removeAttributeNS(attr.namespaceURI, attr.localName)
            for (const attr of replacement.attributes) {
                if (attr.namespaceURI) target.setAttributeNS(attr.namespaceURI, attr.name, attr.value)
                else target.setAttribute(attr.name, attr.value)
            }
        }
        const updateOutput = (source, replacement) => {
            // CFI-resolved selections/annotation ranges address the container's
            // boundaries. Keep that container and its display SVG in place.
            const current = source.output
            const children = Array.from(current.childNodes)
            const newChildren = Array.from(replacement.childNodes)
            attributes(current, replacement)
            // Speech enrichment adds mjx-speech alongside SVG. Preserve both
            // direct children so ranges spanning the formula do not collapse.
            if (children.length === newChildren.length && children.every((node, i) =>
                node.nodeType === newChildren[i].nodeType && node.nodeName === newChildren[i].nodeName)) {
                children.forEach((node, i) => {
                    if (node.nodeType === Node.ELEMENT_NODE) {
                        attributes(node, newChildren[i])
                        node.replaceChildren(...newChildren[i].childNodes)
                    } else node.nodeValue = newChildren[i].nodeValue
                })
            } else current.replaceChildren(...replacement.childNodes)
            retain(current, source)
        }
        const fontCache = () => {
            const doc = mathjax.startup.document
            // convertPromise() does not update the document. Its SVG needs
            // MathJax's normal block, alignment and inline wrapping styles.
            doc.addStyleSheet()
            const cache = doc.outputJax.pageElements?.(doc)
            if (cache) document.body.prepend(cache)
            const global = document.getElementById('MJX-SVG-global-cache')
            if (global && global !== originalCache) global.setAttribute('data-arianna-math-output', '')
        }
        // Convert without letting MathJax replace the original document. Commit
        // wrappers only after every asynchronous conversion has completed.
        for (const source of sources) {
            bootstrap.setAttribute('data-arianna-math-phase', 'convert')
            source.context = source.node.parentElement
            source.output = await convert(source)
            // Conversion items are not in document.math. Keep their expanded
            // MathML so custom TeX macros need not run again in the preview.
            const item = mathjax.startup.document.outputJax.math
            source.mathml = mathjax.startup.document.menu.toMML(item)
        }
        bootstrap.setAttribute('data-arianna-math-phase', 'speech')
        await deadline(mathjax.whenReady(() => {}))
        const style = create('style')
        style.textContent = '[data-arianna-math-source]{display:none!important}[data-arianna-math-wrapper]{display:contents}'
        head.append(style)
        if (!style.sheet) throw new Error('The EPUB policy does not allow math layout styles')
        // Work backwards so multiple matches retain their original text offsets.
        for (const source of sources.reverse()) {
            const wrapper = create('span')
            wrapper.setAttribute('data-arianna-math-wrapper', '')
            const output = source.output
            retain(output, source)
            if (source.node.nodeType === Node.TEXT_NODE) {
                const text = source.node.splitText(source.start)
                text.splitText(source.length)
                const original = create('span')
                original.setAttribute('data-arianna-math-source', '')
                text.before(wrapper)
                original.append(text)
                wrapper.append(original, output)
            } else {
                source.node.before(wrapper)
                source.node.setAttribute('data-arianna-math-source', '')
                wrapper.append(source.node, output)
            }
        }
        fontCache()
        // Display breaks depend on the actual column width. Re-render only
        // reader output from captured MathML, leaving source nodes/CFIs intact.
        let layout = Promise.resolve()
        let requestedWidth = 0
        window.ariannaMathLayout = width => {
            width = Math.round(width)
            if (!rendering || !sources.some(source => source.display) || !(width > 0) || width === requestedWidth) return layout
            requestedWidth = width
            layout = layout.then(async () => {
                if (width !== requestedWidth) return
                for (const source of sources.filter(source => source.display)) {
                    const output = await convert(source, width)
                    updateOutput(source, output)
                }
                fontCache()
                await deadline(mathjax.whenReady(() => {}))
                await document.fonts.ready
            })
            return layout
        }
        await document.fonts.ready
    }
    window.ariannaMathReady = initialize().then(() => {
        bootstrap.setAttribute('data-arianna-math-status', 'ready')
    }).catch(error => {
        // CSP or disabled EPUB scripting is never bypassed. Preserve the source
        // and let the renderer finish with untypeset content on failure.
        bootstrap.setAttribute('data-arianna-math-status', 'failed')
        bootstrap.setAttribute('data-arianna-math-error', String(error))
        console.warn('Arianna MathJax:', bootstrap.getAttribute('data-arianna-math-phase'), error)
    })
})()
