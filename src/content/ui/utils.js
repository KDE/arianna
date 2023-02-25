// SPDX-FileCopyrightText: 2022 John Factotum <50942278+johnfactotum@users.noreply.github.com>
// SPDX-License-Identifier: GPL-3.0-or-later

const XMLNS_NS = 'http://www.w3.org/2000/xmlns/'
const XLINK_NS = 'http://www.w3.org/1999/xlink'
const XHTML_NS = 'http://www.w3.org/1999/xhtml'

const EPUB_NS = 'http://www.idpf.org/2007/ops'

const OPDS_CAT_NS = 'http://opds-spec.org/2010/catalog'
const OPDS_ROOT_NS = 'http://opds-spec.org/'
const OPDS_NS = [OPDS_CAT_NS, OPDS_ROOT_NS]
const THR_NS = 'http://purl.org/syndication/thread/1.0'

const DC_ELS_NS = 'http://purl.org/dc/elements/1.1/'
const DC_TERMS_NS = 'http://purl.org/dc/terms/'
const DC_NS = [DC_TERMS_NS, DC_ELS_NS]

const debounce = (f, wait, immediate) => {
    let timeout
    return (...args) => {
        const later = () => {
            timeout = null
            if (!immediate) f(...args)
        }
        const callNow = immediate && !timeout
        clearTimeout(timeout)
        timeout = setTimeout(later, wait)
        if (callNow) f(...args)
    }
}

const isExternalURL = href => {
    if (href.startsWith('blob:')) return false
    return href.startsWith('mailto:') || href.includes('://')
}

const resolveURL = (url, relativeTo) => {
    // HACK-ish: abuse the URL API a little to resolve the path
    // the base needs to be a valid URL, or it will throw a TypeError,
    // so we just set a random base URI and remove it later
    const base = 'https://example.invalid/'
    return new URL(url, base + relativeTo).href.replace(base, '')
}

const trim = x => x ? x.trim() : x

const unescapeHTML = str => {
    const textarea = document.createElement('textarea')
    textarea.innerHTML = str
    return textarea.value
}

// Remove whitespace like CSS `white-space: normal`
const whitespaceNormal = str =>
    str ? str.replace(/\r?\n/g, ' ').replace(/(\s){2,}/g, ' ') : ''

// from https://stackoverflow.com/a/11892228
const usurp = p => {
    let last = p
    for (let i = p.childNodes.length - 1; i >= 0; i--) {
        let e = p.removeChild(p.childNodes[i])
        p.parentNode.insertBefore(e, last)
        last = e
    }
    p.parentNode.removeChild(p)
}
const pangoMarkupTags = ['a', 'b', 'big', 'i', 's', 'sub', 'sup', 'small', 'tt', 'u']
const toPangoMarkup = (html, baseURL = '') => {
    const isBaseURLExternal = isExternalURL(baseURL)
    html = whitespaceNormal(html)
    const doc = new DOMParser().parseFromString(html, 'text/html')
    Array.from(doc.querySelectorAll('em'))
        .forEach(el => el.innerHTML = '<i>' + el.innerHTML + '</i>')
    Array.from(doc.querySelectorAll('strong'))
        .forEach(el => el.innerHTML = '<b>' + el.innerHTML + '</b>')
    Array.from(doc.querySelectorAll('code'))
        .forEach(el => el.innerHTML = '<tt>' + el.innerHTML + '</tt>')
    Array.from(doc.body.querySelectorAll('*')).forEach(el => {
        const nodeName = el.nodeName.toLowerCase()
        if (pangoMarkupTags.indexOf(nodeName) === -1) usurp(el)
        else Array.from(el.attributes).forEach(x => {
            if (x.name === 'href') {
                if (baseURL) {
                    const href = el.getAttribute('href')
                    if (isBaseURLExternal)
                        el.setAttribute('href', new URL(href, baseURL))
                    else
                        el.setAttribute('href', resolveURL(href, baseURL))
                }
            } else el.removeAttribute(x.name)
        })
        if (nodeName === 'a' && !el.hasAttribute('href')) usurp(el)
    })
    return unescapeHTML(doc.body.innerHTML.replace(/&lt;/g, '&amp;lt;').trim()).replace(/&/g, '&amp;')
}
