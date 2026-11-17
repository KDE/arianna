// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: MIT

// Reuse the source view's current book and reader CSS. Presentation state stays
// in memory and can also be replaced in an open preview without reloading it.
export const createFootnoteAppearance = (doc, readerStyle = {}) => {
    const styles = [...doc.head.querySelectorAll('style, link[rel="stylesheet"]')].map(el =>
        el.localName === 'link' ? { href: el.href, media: el.media }
            : { css: el.textContent, media: el.media })
    const root = doc.defaultView?.getComputedStyle(doc.documentElement)
    const body = doc.defaultView?.getComputedStyle(doc.body)
    const transparent = color => !color || color === 'rgba(0, 0, 0, 0)' || color === 'transparent'
    const theme = readerStyle.theme
    const colors = readerStyle.invert ? theme?.inverted
        : doc.defaultView?.matchMedia('(prefers-color-scheme: dark)').matches ? theme?.dark : theme?.light
    const backgroundColor = !transparent(body?.backgroundColor) ? body.backgroundColor
        : !transparent(root?.backgroundColor) ? root.backgroundColor : colors?.bg || '#ffffff'
    const foregroundColor = colors?.fg || root?.color || '#000000'
    const image = readerStyle.readerBackgroundImage
    styles.push({ css: `
        html, body {
            position: static !important; inset: auto !important;
            width: auto !important; min-width: 0 !important; max-width: none !important;
            height: auto !important; min-height: 0 !important; max-height: none !important;
            columns: auto !important; transform: none !important;
            overflow: visible !important; margin: 0 !important;
        }
        html {
            min-height: 100vh !important;
            isolation: isolate;
            background-color: ${backgroundColor} !important;
            scrollbar-color: auto !important; scrollbar-width: auto !important;
            ${root ? `font-family: ${root.fontFamily}; font-size: ${root.fontSize};` : ''}
        }
        body { padding: 12px !important; overflow-wrap: anywhere }
        img, svg { max-width: 100% }
        /* Chromium draws this scrollbar, outside the Kirigami controls. */
        html::-webkit-scrollbar {
            width: 12px !important; height: 12px !important;
            background: transparent !important;
        }
        html::-webkit-scrollbar-track {
            background-color: ${backgroundColor} !important;
            background-image: ${image || 'none'} !important;
            background-position: center center !important;
            background-size: cover !important;
            background-repeat: no-repeat !important;
        }
        html::-webkit-scrollbar-corner {
            background: transparent !important;
        }
        html::-webkit-scrollbar-thumb {
            min-height: 32px !important;
            border: 3px solid transparent !important;
            border-radius: 6px !important;
            background: color-mix(in srgb, ${foregroundColor} 45%, transparent) !important;
            background-clip: padding-box !important;
        }
        html::-webkit-scrollbar-thumb:hover {
            background-color: color-mix(in srgb, ${foregroundColor} 65%, transparent) !important;
        }
        html::-webkit-scrollbar-thumb:active {
            background-color: color-mix(in srgb, ${foregroundColor} 80%, transparent) !important;
        }
        html::-webkit-scrollbar-button {
            display: none !important; width: 0 !important; height: 0 !important;
        }
        ${image ? `
        html { background-image: none !important }
        body { background: transparent !important }
        html::before {
            content: "" !important;
            position: fixed !important; inset: 0 !important;
            z-index: -1 !important; pointer-events: none !important;
            background-image: ${image} !important;
            background-position: center center !important;
            background-size: cover !important;
            background-repeat: no-repeat !important;
            filter: ${readerStyle.readerBackgroundFilter || 'none'} !important;
        }` : ''}
    ` })
    return { styles, backgroundColor }
}

// A temporary presentation copy, never a book edit. Keep resource URLs alive
// in the source foliate-view until the native window releases this snapshot.
export const createFootnotePreview = (doc, book, index, requestId, resourceOrigin = '', readerStyle = {}) => {
    const preview = doc.implementation.createHTMLDocument('')
    preview.documentElement.dataset.ariannaFootnoteRequest = requestId
    for (const attr of ['lang', 'dir']) {
        preview.documentElement.setAttribute(attr, doc.documentElement.getAttribute(attr) || '')
    }
    const policy = preview.createElement('meta')
    policy.httpEquiv = 'Content-Security-Policy'
    // This endpoint comes from the native reader configuration, never the EPUB.
    let assets = ''
    try {
        const url = new URL(resourceOrigin)
        if (['http:', 'https:'].includes(url.protocol)) assets = ' ' + url.origin
    } catch {}
    policy.content = `default-src 'none'; script-src 'none'; style-src 'unsafe-inline' blob: data: qrc:${assets}; `
        + `img-src blob: data: qrc:${assets}; font-src blob: data: qrc:${assets}; object-src 'none'; `
        + "frame-src 'none'; base-uri 'none'; form-action 'none'"
    preview.head.prepend(policy)
    const charset = preview.createElement('meta')
    charset.setAttribute('charset', 'utf-8')
    preview.head.prepend(charset)
    const appearance = createFootnoteAppearance(doc, readerStyle)
    for (const style of appearance.styles) {
        const el = preview.createElement(style.href ? 'link' : 'style')
        el.dataset.ariannaFootnoteStyle = ''
        if (style.href) {
            el.rel = 'stylesheet'
            el.href = style.href
        } else el.textContent = style.css
        if (style.media) el.media = style.media
        preview.head.append(el)
    }
    preview.body.replaceWith(doc.body.cloneNode(true))
    for (const el of preview.querySelectorAll('script, iframe, frame, object, embed, form, input, button, textarea, select, base, meta[http-equiv="refresh"]')) el.remove()
    for (const el of preview.querySelectorAll('*')) {
        for (const attr of [...el.attributes]) {
            if (/^on/i.test(attr.name) || ['autofocus', 'contenteditable'].includes(attr.name.toLowerCase()))
                el.removeAttribute(attr.name)
        }
    }
    const links = []
    for (const link of preview.body.querySelectorAll('a[href]')) {
        const raw = link.getAttribute('href')
        if (!raw || /^(?:javascript|vbscript|data|file|blob):/i.test(raw.trim())) {
            link.removeAttribute('href')
            continue
        }
        const href = book.sections[index]?.resolveHref?.(raw) ?? raw
        link.removeAttribute('target')
        if (!href || /^(?:javascript|vbscript|data|file|blob):/i.test(href)) {
            link.removeAttribute('href')
            continue
        }
        links.push({ href, external: !!book.isExternal?.(href) })
        link.setAttribute('href', `https://arianna.invalid/footnote/${encodeURIComponent(requestId)}/${links.length - 1}`)
    }
    return { html: '<!doctype html>' + preview.documentElement.outerHTML, links, appearance }
}
