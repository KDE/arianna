// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Only scalar reader preferences, never executable author configuration.
(() => {
    const pick = (value, choices, fallback) => choices.includes(value) ? value : fallback
    const size = (value, fallback) => Number.isFinite(Number(value))
        ? Math.min(2, Math.max(.5, Number(value))) : fallback
    const normalize = (value = {}) => {
        if (!value || typeof value !== 'object') value = {}
        const scale = size(value.scale, 1)
        return ({
            scale,
            // Older delivery requests supplied a single size for all math.
            inlineScale: size(value.inlineScale, scale),
            align: pick(value.align, ['left', 'center', 'right'], 'center'),
            overflow: pick(value.overflow, ['linebreak', 'scale', 'scroll', 'overflow'], 'linebreak'),
            inline: value.inline !== false,
            fontCache: pick(value.fontCache, ['local', 'global', 'none'], 'local'),
        })
    }
    const configure = (mathjax, value, display = true) => {
        const options = normalize(value)
        const output = mathjax.startup.document.outputJax
        Object.assign(output.options, {scale: display ? options.scale : options.inlineScale, displayAlign: options.align,
            displayOverflow: options.overflow})
        output.options.linebreaks.inline = options.inline
        if (output.name === 'SVG' && output.options.fontCache !== options.fontCache) {
            output.options.fontCache = options.fontCache
            // Global SVG caching is initialized at startup only when it was
            // selected then. A later change needs a fresh defs element too.
            output.clearFontCache()
        }
        return options
    }
    window.ariannaMathDisplay = Object.freeze({normalize, configure})
})()
