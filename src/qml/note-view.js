// SPDX-License-Identifier: GPL-3.0-or-later
(() => {
    const tags = new Set('div p h1 h2 h3 h4 h5 h6 strong em del code pre blockquote ul ol li table thead tbody tfoot tr th td span a br hr'.split(' '))
    const copy = node => {
        if (node.nodeType === Node.TEXT_NODE) return document.createTextNode(node.data)
        if (node.nodeType !== Node.ELEMENT_NODE || !tags.has(node.localName)) return null
        const el = document.createElement(node.localName)
        if (node.localName === 'span' && ['math-source', 'math-pending'].includes(node.className)) el.className = node.className
        if (node.localName === 'div' || node.localName === 'p') {
            const classes = [...node.classList].filter(name =>
                ['chat-conversation', 'chat-message', 'chat-user', 'chat-assistant', 'chat-role', 'chat-status'].includes(name))
            if (classes.length) el.className = classes.join(' ')
        }
        for (const attr of ['rowspan', 'colspan', 'start'])
            if (/^\d{1,4}$/.test(node.getAttribute(attr) || '')) el.setAttribute(attr, node.getAttribute(attr))
        if (node.localName === 'a') {
            try {
                const url = new URL(node.getAttribute('href'))
                if (['https:', 'http:'].includes(url.protocol)) { el.href = url.href; el.rel = 'noreferrer' }
            } catch {}
        }
        for (const child of node.childNodes) { const safe = copy(child); if (safe) el.append(safe) }
        return el
    }
    let ready
    const mathReady = () => {
        if (ready) return ready
        const base = 'qrc:/arianna/mathjax/'
        window.MathJax = {
            loader: { paths: {mathjax:base, fonts:base+'fonts'}, load:['ui/safe'] },
            startup: {typeset:false},
            tex: {packages:['base','ams','newcommand','noundefined','configmacros','textmacros'],
                inlineMath:[['\\(','\\)']], displayMath:[['\\[','\\]'],['$$','$$']], maxBuffer:10000, maxMacros:1000},
            svg: {fontCache:'local'},
            output: {font:'mathjax-newcm',fontPath:base+'fonts/%%FONT%%-font'},
            options: {enableMenu:false, enableEnrichment:false, enableComplexity:false,
                enableExplorer:false, enableSpeech:false, enableBraille:false, enableExplorerHelp:false,
                menuOptions:{settings:{enrich:false,speech:false,braille:false,collapsible:false,help:false}},
                safeOptions:{allow:{URLs:'none',classes:'none',cssIDs:'none',styles:'none'}}},
        }
        ready = new Promise((resolve,reject) => {
            const script = document.createElement('script')
            script.src = base+'tex-mml-svg.js'
            script.onload = () => MathJax.startup.promise.then(resolve,reject)
            script.onerror = () => reject(Error('Local MathJax could not be loaded'))
            document.head.append(script)
        })
        return ready
    }
    let serial = 0
    let queue = Promise.resolve()
    window.showNote = html => {
        const current = ++serial
        queue = queue.catch(() => {}).then(async () => {
            if (current !== serial) return
            const target = document.getElementById('note')
            window.MathJax?.typesetClear?.([target])
            const doc = new DOMParser().parseFromString(html, 'text/html')
            target.replaceChildren()
            for (const child of doc.body.childNodes) { const safe = copy(child); if (safe) target.append(safe) }
            window.noteStatus = 'ready'
            if (target.querySelector('.math-source')) {
                window.noteStatus = 'loading'
                try {
                    await mathReady()
                    if (current !== serial) return
                    await MathJax.typesetPromise([target])
                    if (current === serial) window.noteStatus = 'ready'
                } catch { if (current === serial) window.noteStatus = 'failed' }
            }
        })
        return queue
    }
})()
