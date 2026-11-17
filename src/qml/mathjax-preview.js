// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// MathJax's menu model lives in this separate document/profile, with native
// Qt UI. Settings and rerenders cannot modify the EPUB or its saved locations.
(() => {
    'use strict'
    const base = new URL('.', document.currentScript.src).href
    const MML = 'http://www.w3.org/1998/Math/MathML'
    window.formulaStatus = 'loading'
    window.formulaSpeech = () => {
        const formula = document.querySelector('#formula mjx-container')
        let text = formula?.getAttribute('data-semantic-speech-none')
            || formula?.getAttribute('data-semantic-speech') || ''
        if (text.includes('<')) {
            const parsed = new DOMParser().parseFromString(text, 'application/xml')
            text = parsed.querySelector('parsererror') ? '' : parsed.documentElement.textContent
        }
        return { text: text.trim(), locale: formula?.getAttribute('data-semantic-locale') || '' }
    }
    window.MathJax = {
        loader: { paths: { mathjax: base.replace(/\/$/, ''), fonts: `${base}fonts`, sre: `${base}sre`, mathmaps: `${base}sre/mathmaps` }, load: ['ui/safe'] },
        startup: { typeset: false },
        tex: { packages: ['base', 'ams', 'newcommand', 'noundefined', 'configmacros', 'textmacros'], maxBuffer: 10000, maxMacros: 1000 },
        mml: { allowHtmlInTokenNodes: false },
        output: { font: 'mathjax-newcm', fontPath: `${base}fonts/%%FONT%%-font` },
        svg: { fontCache: 'local' },
        chtml: { fontURL: `${base}fonts/mathjax-newcm-font/chtml/woff2` },
        options: {
            enableMenu: true,
            worker: { path: `${base}sre`, maps: `${base}sre/mathmaps` },
            menuOptions: { settings: { renderer: 'SVG', zoom: 'NoZoom' } },
            safeOptions: { allow: { URLs: 'none', classes: 'none', cssIDs: 'none', styles: 'none' } },
        },
    }
    const ready = new Promise((resolve, reject) => {
        const script = document.createElement('script')
        script.src = `${base}tex-mml-svg.js`
        script.onload = () => MathJax.startup.promise.then(() => {
            installFormulaNativeMenu()
            resolve()
        }, reject).catch(reject)
        script.onerror = () => reject(new Error('Cannot load local MathJax'))
        document.head.append(script)
    })
    // Never insert author HTML, event handlers, URLs or annotation-xml into
    // this reader-owned document. TeX is data interpreted by fixed packages.
    const names = new Set(('math mi mn mo mtext ms mspace mrow mfrac msqrt mroot mstyle merror mpadded mphantom mfenced menclose '
        + 'msub msup msubsup munder mover munderover mmultiscripts mprescripts none mtable mtr mlabeledtr mtd maligngroup malignmark maction semantics annotation').split(' '))
    const attributes = new Set(('display mathvariant mathsize mathcolor mathbackground dir displaystyle scriptlevel '
        + 'accent accentunder align bevelled close columnalign columnlines columnspacing columnspan depth fence form frame framespacing '
        + 'height largeop length linethickness lspace maxsize minsize movablelimits notation open rowalign rowlines rowspacing rowspan '
        + 'rspace separator separators stretchy subscriptshift superscriptshift symmetric width encoding').split(' '))
    const copyMath = node => {
        if (node.nodeType === Node.TEXT_NODE) return document.createTextNode(node.data)
        if (node.nodeType !== Node.ELEMENT_NODE || node.namespaceURI !== MML || !names.has(node.localName)) return null
        const el = document.createElementNS(MML, node.localName)
        for (const attr of node.attributes)
            if (!attr.namespaceURI && attributes.has(attr.name)) el.setAttribute(attr.name, attr.value)
        for (const child of node.childNodes) {
            const copy = copyMath(child)
            if (copy) el.append(copy)
        }
        return el
    }
    let serial = 0
    let rendering = Promise.resolve()
    window.showFormula = (details, request = serial + 1) => {
        const current = ++serial
        window.formulaRequest = request
        window.formulaStatus = 'loading'
        window.formulaError = ''
        resetFormulaNativeMenu()
        rendering = rendering.then(() => renderFormula(details, current))
        return rendering
    }
    const renderFormula = async (details, current) => {
        try {
            await ready
            if (current !== serial) return
            const container = document.getElementById('formula')
            MathJax.typesetClear([container])
            container.replaceChildren()
            const doc = MathJax.startup.document
            if (details.rendering) {
                const cache = doc.outputJax.options.fontCache
                ariannaMathDisplay.configure(MathJax, details.rendering, !!details.display)
                Object.assign(doc.menu.settings, {scale: doc.outputJax.options.scale,
                    overflow: doc.outputJax.options.displayOverflow.replace(/^./, letter => letter.toUpperCase()),
                    breakInline: doc.outputJax.options.linebreaks.inline})
                if (cache !== doc.outputJax.options.fontCache)
                    document.getElementById('MJX-SVG-global-cache')?.remove()
            }
            const locale = ['en', 'de', 'fr', 'es', 'it', 'hi', 'ko', 'nb', 'nn', 'sv'].includes(details.locale) ? details.locale : 'en'
            doc.options.sre.locale = doc.menu.settings.locale = locale
            // ClearSpeak's explicit preferences avoid verbose numerator /
            // denominator narration and name ellipses as "and so on".
            const style = details.concise === false ? 'default' : 'Fraction_Over:Ellipses_AndSoOn'
            doc.options.sre.domain = 'clearspeak'
            doc.options.sre.style = style
            doc.menu.settings.speechRules = doc.options.a11y.speechRules = `clearspeak-${style}`
            if (details.mathml) {
                const parsed = new DOMParser().parseFromString(details.mathml, 'application/xml')
                const math = copyMath(parsed.documentElement)
                if (!math || math.localName !== 'math') throw new Error('Invalid formula MathML')
                math.setAttribute('display', details.display ? 'block' : 'inline')
                // The native menu can show/copy the original TeX annotation.
                if (details.tex) {
                    const semantics = document.createElementNS(MML, 'semantics')
                    const row = document.createElementNS(MML, 'mrow')
                    row.append(...math.childNodes)
                    const annotation = document.createElementNS(MML, 'annotation')
                    annotation.setAttribute('encoding', 'application/x-tex')
                    annotation.textContent = details.tex
                    semantics.append(row, annotation)
                    math.append(semantics)
                }
                container.append(math)
            } else if (details.tex) {
                container.textContent = details.display ? `\\[${details.tex}\\]` : `\\(${details.tex}\\)`
            } else throw new Error('Formula source is unavailable')
            await MathJax.typesetPromise([container])
            await MathJax.whenReady(() => {})
            await document.fonts.ready
            if (current === serial) window.formulaStatus = 'ready'
        } catch (error) {
            if (current === serial) {
                window.formulaError = String(error)
                window.formulaStatus = 'failed'
            }
        }
    }
})()
