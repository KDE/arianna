// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Adapter for the pinned MathJax 4.1 menu model. Only reader-owned preview
// code calls this bridge; no author configuration or executable strings cross it.
(() => {
    'use strict'
    let revision = 0
    let nextToken = 0
    let actions = new Map()
    let events = []
    let copying = false
    let actionRevision = null
    let pending = Promise.resolve()
    const emit = event => {
        if (actionRevision === null || actionRevision === revision)
            events.push({ ...event, request: window.formulaRequest, revision })
    }
    const plain = html => {
        const doc = new DOMParser().parseFromString(String(html), 'text/html')
        for (const el of doc.querySelectorAll('br, hr')) el.replaceWith('\n')
        for (const el of doc.querySelectorAll('p')) el.append('\n\n')
        return doc.body.textContent.trim()
    }
    const register = action => {
        const token = String(++nextToken)
        actions.set(token, action)
        return token
    }
    const activeMenu = () => {
        const owner = MathJax.startup.document.menu
        owner.menu.mathItem = Array.from(MathJax.startup.document.math)[0]
        return owner.menu
    }
    const finish = async () => {
        await MathJax.startup.document.menu.loadingPromise
        await MathJax.whenReady(() => {})
        await document.fonts.ready
    }
    const dialog = args => {
        if (copying) {
            emit({ kind: 'copy', text: String(args.message).trim() })
        } else {
            emit({ kind: 'dialog', title: plain(args.title), code: !!args.code,
                text: args.code ? String(args.message) : plain(args.message),
                links: (args.extraNodes || []).map(node => node.getAttribute('href')).filter(url => /^https?:/.test(url)) })
        }
    }
    const preferences = async menu => {
        const locales = new Map()
        await menu.mathItem.generatorPool.getLocalePreferences(locales)
        const preferences = locales.get(menu.settings.locale) || {}
        emit({ kind: 'preferences', title: 'ClearSpeak Preferences',
            items: Object.keys(preferences).sort().map(name => {
                const variable = menu.pool.lookup(`csprf_${name}`)
                const choices = preferences[name].map(value => value.replace(`${name}_`, ''))
                return { label: name, choices, value: variable.getValue(),
                    token: register({ variable, choices }) }
            }) })
    }
    const serialize = (menu, path = []) => menu.items.filter(item => !item.hidden).map(item => {
        const ids = [...path, item.id || item.type]
        const variable = item.variable
        const value = variable?.getValue()
        return {
            id: item.id || '', path: ids, type: item.type,
            text: String(item.content || '').trim(), enabled: !item.disabled,
            checked: item.type === 'checkbox' ? !!value
                : !!variable && (item.comparator ? item.comparator(value, item.id) : value === item.id),
            value, variable: variable?.name || '',
            token: item.type === 'submenu' || item.type === 'rule' || item.type === 'label' ? ''
                : register({ item, path: ids }),
            children: item.submenu ? serialize(item.submenu, ids) : [],
        }
    })
    window.installFormulaNativeMenu = () => {
        const owner = MathJax.startup.document.menu
        const menu = owner.menu
        // MathJax keeps its menu/settings model and formula interaction, but
        // posting any HTML context menu is replaced by a request for Qt UI.
        menu.post = () => window.openFormulaMenu()
        // CopyDialog.post delegates to InfoDialog.post in this version.
        MathJax._.ui.dialog.InfoDialog.InfoDialog.post = dialog
        owner.scaleAllMath = () => emit({ kind: 'slider', title: 'Scale All Math',
            min: 0.1, max: 1000, value: Number(owner.settings.scale) * 100,
            token: register({ variable: menu.pool.lookup('scale'), scale: true }) })
        owner.zoomBox = () => {
            const node = activeMenu().mathItem.typesetRoot
            const cache = document.getElementById('MJX-SVG-global-cache')?.outerHTML || ''
            const css = Array.from(document.querySelectorAll('style')).map(style => style.textContent).join('\n')
            // A native dialog owns this view. Its content is the already
            // typeset, safe formula, with scripts and network access disabled.
            emit({ kind: 'zoom', title: 'MathJax Zoomed Expression', html:
                '<!doctype html><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" '
                + 'content="default-src \'none\'; style-src \'unsafe-inline\'; img-src data:; font-src \'self\' data:">'
                + '<style>:root{color-scheme:light dark}body{margin:24px}' + css + '</style>'
                + cache + '<div style="font-size:' + getComputedStyle(node).fontSize + ';zoom:'
                + parseFloat(owner.settings.zscale) / 100 + '">' + node.outerHTML + '</div>' })
        }
    }
    window.resetFormulaNativeMenu = () => {
        ++revision
        actions.clear()
        events = []
    }
    window.openFormulaMenu = () => {
        const request = window.formulaRequest
        pending = pending.then(async () => {
            await finish()
            if (request !== window.formulaRequest || window.formulaStatus !== 'ready') return
            const menu = activeMenu()
            if (!menu.mathItem) return
            ++revision
            const menuRevision = revision
            actions = new Map()
            // The same availability checks used by MJContextMenu.post().
            for (const name of ['getOriginalMenu', 'getSemanticsMenu', 'getSpeechMenu',
                'getBrailleMenu', 'getSvgMenu', 'getErrorMessage']) menu[name]()
            // Dynamic speech/annotation menus must finish before Qt sees them.
            const dynamic = menu.constructor.DynamicSubmenus
            for (const [id, [build, option]] of dynamic) {
                const item = menu.find(id)
                if (!item) continue
                await new Promise(resolve => {
                    const result = build(menu, item, submenu => {
                        item.submenu = submenu
                        submenu?.items?.length && (!option || menu.settings[option]) ? item.enable() : item.disable()
                        resolve()
                    })
                    Promise.resolve(result).catch(resolve)
                })
            }
            if (revision === menuRevision) emit({ kind: 'menu', items: serialize(menu) })
        }).catch(error => emit({ kind: 'error', text: String(error) }))
        return pending
    }
    window.formulaNativeAction = (token, value, expectedRevision, expectedRequest = window.formulaRequest) => {
        pending = pending.then(async () => {
            if (expectedRevision !== revision || expectedRequest !== window.formulaRequest) return
            const action = actions.get(String(token))
            if (!action || action.item?.disabled || action.item?.hidden) return
            const menu = activeMenu()
            if (!menu.mathItem) return
            actionRevision = expectedRevision
            const request = window.formulaRequest
            window.formulaStatus = 'loading'
            try {
                if (action.variable) {
                    if (action.choices) {
                        if (!action.choices.includes(value)) return
                    } else {
                        value = Number(value)
                        if (!Number.isFinite(value) || value < (action.scale ? 0.1 : 0)
                            || value > (action.scale ? 1000 : 100)) return
                        value = String(action.scale ? value / 100 : value)
                    }
                    action.variable.setValue(value)
                } else if (action.item.type === 'slider') {
                    emit({ kind: 'slider', title: action.item.variable.name === 'backgroundOpacity'
                        ? 'Background opacity' : 'Foreground opacity', min: 0, max: 100,
                        value: Number(action.item.variable.getValue()), token: register({ variable: action.item.variable }) })
                } else if (action.item.type === 'checkbox' || action.item.type === 'radio') {
                    action.item.variable.setValue(action.item.type === 'checkbox' ? !action.item.variable.getValue() : action.item.id)
                    action.item.executeCallbacks_()
                } else if (action.item.id === 'ClearspeakPreferences') {
                    await preferences(menu)
                } else if (action.item.type === 'command') {
                    if (action.path[0] === 'Copy') {
                        // Use the matching Show action so all copies, including
                        // dynamic annotations, use Qt's clipboard without web UI.
                        const ids = action.path.slice(1).map(id => id === 'CopyAnnotation' ? 'ShowAnnotation' : id)
                        copying = true
                        try { await menu.findID('Show', ...ids).command() } finally { copying = false }
                    } else {
                        await action.item.command()
                    }
                    action.item.executeCallbacks_()
                }
                await finish()
            } catch (error) {
                emit({ kind: 'error', text: String(error) })
            } finally {
                actionRevision = null
                if (request === window.formulaRequest) window.formulaStatus = 'ready'
            }
        })
        return pending
    }
    window.formulaNativeState = () => {
        const result = events
        events = []
        const settings = MathJax.startup?.document?.menu?.settings
        return { events: result, locale: settings?.locale, speechRules: settings?.speechRules }
    }
})()
