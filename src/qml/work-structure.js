// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Consumer helpers for a validated BookServer record; no inference or storage.
export const semanticPath = (record, node) => (node.coordinate || []).map((number, i) => ({
    ...(record.levels?.[i] || {}), number,
}))
export const normalizedDigits = text => String(text).replace(/[०-९]/g, c => String(c.charCodeAt(0) - 0x0966))
export const semanticReferenceTitle = location => {
    if (!Array.isArray(location?.semanticPath) || !location.semanticPath.length) return ''
    return location.semanticPath.map(part => `${part.iast} ${part.number}`).join(', ')
}
