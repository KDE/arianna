// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick

ListModel {
    id: root

    property bool loading: false

    signal searchTriggered(text: string)

    function search(text) {
        loading = false;
        searchTriggered(text);
    }

    function resultFound(query, results) {
        clear();
        loading = false;

        const markupEscape = text => text ? text.replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;") : '';
        const regexEscape = str => str ? str.replace(/[.*+\-?^${}()|[\]\\]/g, '\\$&') : '';
        const regex = new RegExp(regexEscape(query), 'ig');
        results.forEach(({ cfi, excerpt, section }) => {
            const { pre, match, post } = excerpt;
            const text = markupEscape((pre + match + post));
            const markup = text.replace(regex, `<strong>$&</strong>`);
            const sectionMarkup = `<span alpha="50%" size="smaller">${
                markupEscape(section)}</span>`

            root.append({
                cfi: cfi,
                markup: markup,
                sectionMarkup: sectionMarkup
            });
        });
    }
}
