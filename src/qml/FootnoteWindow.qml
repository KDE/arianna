// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtWebEngine
import org.kde.kirigami as Kirigami

Window {
    id: window
    objectName: "epubFootnoteWindow"

    required property WebEngineProfile readerProfile
    property bool readerActive: true
    property real readerZoom: 1
    property var details: null
    property bool positioned: false
    property bool loadFailed: false
    readonly property int fullHeight: Kirigami.Units.gridUnit * 26
    property var lastMeasurement: null
    readonly property string requestId: details?.requestId || ""
    readonly property string linkPrefix: "https://arianna.invalid/footnote/" + encodeURIComponent(requestId) + "/"
    readonly property alias preview: browser

    signal dismissed(string requestId)
    signal goToRequested(string requestId)
    signal linkActivated(string requestId, int linkIndex)

    visible: false
    title: details?.title || i18n("Footnote")
    modality: Qt.NonModal
    flags: Qt.Tool | Qt.WindowTitleHint | Qt.WindowCloseButtonHint
    color: Kirigami.Theme.backgroundColor
    width: Kirigami.Units.gridUnit * 32
    height: fullHeight
    minimumWidth: Kirigami.Units.gridUnit * 18
    minimumHeight: Kirigami.Units.gridUnit * 4

    function fitContentHeight() {
        if (!requestId || browser.loading || loadFailed) return;
        const id = requestId;
        const measuredWidth = browser.width;
        const zoom = readerZoom;
        browser.runJavaScript(`(() => {
            if (document.documentElement.dataset.ariannaFootnoteRequest !== ${JSON.stringify(id)}) return null;
            const body = document.body;
            return Math.max(body.getBoundingClientRect().height, body.scrollHeight)
                + innerHeight - document.documentElement.clientHeight;
        })()`, 1, naturalHeight => {
            if (id !== window.requestId || measuredWidth !== browser.width || zoom !== window.readerZoom
                || !Number.isFinite(naturalHeight) || naturalHeight <= 0) return;
            // A viewport-only change can come from a manual height adjustment.
            // Refit only for a new note, new width/zoom, or changed content.
            const previous = window.lastMeasurement;
            if (previous?.requestId === id && previous.width === measuredWidth && previous.zoom === zoom
                && Math.abs(previous.height - naturalHeight) < 1) return;
            window.lastMeasurement = {requestId: id, width: measuredWidth, zoom: zoom, height: naturalHeight};
            const controlsHeight = window.height - browser.height;
            window.height = Math.min(window.fullHeight, Math.max(window.minimumHeight,
                Math.ceil(naturalHeight * zoom + controlsHeight)));
        });
    }

    onWidthChanged: heightFit.restart()
    onReaderZoomChanged: heightFit.restart()

    Timer {
        id: heightFit
        interval: 50
        onTriggered: window.fitContentHeight()
    }

    function open(note) {
        if (!readerActive || !note?.requestId || !note.html) return;
        details = note;
        loadFailed = false;
        lastMeasurement = null;
        if (!positioned && transientParent) {
            x = transientParent.x + Math.max(0, (transientParent.width - width) / 2);
            y = transientParent.y + Kirigami.Units.gridUnit * 3;
            positioned = true;
        }
        browser.loadHtml(note.html, note.baseUrl);
        show();
        requestActivate();
    }

    function reset(id) {
        if (id && id !== requestId) return;
        close();
        details = null;
        browser.url = "about:blank";
        loadFailed = false;
    }

    function applyAppearance(appearance) {
        if (!appearance || !requestId || appearance.requestId !== requestId) return;
        details = Object.assign({}, details, {appearance: appearance});
        // Update CSS in the isolated native world; keep DOM, scroll and focus.
        // Page scripts remain disabled, and the request guard covers load races.
        browser.runJavaScript(`(() => {
            const appearance = ${JSON.stringify(appearance)};
            if (document.documentElement.dataset.ariannaFootnoteRequest !== appearance.requestId) return;
            const x = scrollX, y = scrollY;
            const fragment = document.createDocumentFragment();
            for (const style of appearance.styles) {
                const el = document.createElement(style.href ? 'link' : 'style');
                el.dataset.ariannaFootnoteStyle = '';
                if (style.href) {
                    el.rel = 'stylesheet';
                    el.href = style.href;
                } else el.textContent = style.css;
                if (style.media) el.media = style.media;
                fragment.append(el);
            }
            for (const el of document.head.querySelectorAll('[data-arianna-footnote-style]')) el.remove();
            document.head.append(fragment);
            scrollTo(x, y);
        })()`, 1, () => heightFit.restart());
    }

    function followLink(url) {
        if (!readerActive || !requestId || !String(url).startsWith(linkPrefix)) return;
        const index = String(url).slice(linkPrefix.length);
        if (/^\d+$/.test(index)) linkActivated(requestId, Number(index));
    }

    onClosing: {
        const id = requestId;
        heightFit.stop();
        lastMeasurement = null;
        details = null;
        browser.url = "about:blank";
        if (id) dismissed(id);
    }
    onReaderActiveChanged: if (!readerActive) reset("")

    Shortcut {
        sequence: "Esc"
        context: Qt.WindowShortcut
        enabled: window.visible
        onActivated: window.close()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: window.loadFailed
            type: Kirigami.MessageType.Error
            text: i18n("The footnote could not be loaded.")
        }

        WebEngineView {
            id: browser
            objectName: "epubFootnotePreview"
            Layout.fillWidth: true
            Layout.fillHeight: true
            url: "about:blank"
            profile: window.readerProfile
            zoomFactor: window.readerZoom
            backgroundColor: window.details?.appearance?.backgroundColor || Kirigami.Theme.backgroundColor

            // The reader supplies an already rendered, read-only DOM snapshot.
            // No book scripts or privileged reader WebChannel run in this view.
            settings.javascriptEnabled: false
            settings.localContentCanAccessFileUrls: false
            // The CSP limits this to the configured BookServer (fonts/background).
            settings.localContentCanAccessRemoteUrls: true
            settings.javascriptCanAccessClipboard: false
            onContentsSizeChanged: heightFit.restart()
            onLoadingChanged: request => {
                if (request.status === WebEngineView.LoadFailedStatus && window.details)
                    window.loadFailed = true;
                else if (request.status === WebEngineView.LoadSucceededStatus)
                    window.applyAppearance(window.details?.appearance);
            }
            onNavigationRequested: request => {
                if (request.navigationType === WebEngineView.LinkClickedNavigation) {
                    request.action = WebEngineView.IgnoreRequest;
                    window.followLink(request.url);
                } else if (request.isMainFrame && String(request.url) !== "about:blank"
                           && String(request.url) !== String(window.details?.baseUrl || "")
                           && !String(request.url).startsWith("data:text/html")) {
                    request.action = WebEngineView.IgnoreRequest;
                }
            }
            onNewWindowRequested: request => {
                if (request.userInitiated) window.followLink(request.requestedUrl);
            }
            onPermissionRequested: permission => permission.deny()
        }

        QQC2.DialogButtonBox {
            Layout.fillWidth: true
            visible: !!window.details && !window.details.hidden
            QQC2.Button {
                objectName: "epubFootnoteGoButton"
                text: window.details?.goLabel || i18n("Go to Footnote")
                QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.ActionRole
                onClicked: if (window.requestId) window.goToRequested(window.requestId)
            }
        }
    }
}
