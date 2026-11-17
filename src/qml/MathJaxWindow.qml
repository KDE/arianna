// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtWebEngine
import org.kde.kirigami as Kirigami
import org.kde.arianna

Window {
    id: window
    objectName: "epubMathJaxWindow"

    required property string runtimeBaseUrl
    property bool readerActive: true
    property var formula: null
    property var renderingOptions: null
    property string status: "loading"
    property int requestSerial: 0
    property bool positioned: false
    property string preferredSpeechLocale: Qt.locale().name
    readonly property var speechLanguages: ["de", "en", "fr", "es", "it", "hi", "ko", "nb", "nn", "sv"]
    property string speechLocale: {
        const language = preferredSpeechLocale.replace("_", "-").split("-")[0].toLowerCase();
        return speechLanguages.includes(language) ? language : "en";
    }
    property bool conciseSpeech: true
    property string speechText: ""
    property string actualSpeechLocale: ""
    property bool synchronizingMenu: false
    property var contextMenuHost: null
    property bool contextMenuPending: false
    property int menuActionSerial: 0
    property bool menuActionPending: false
    readonly property alias previewProfile: formulaProfile
    readonly property var preview: previewLoader.item
    signal readAloudRequested(var segment)
    readonly property string previewUrl: runtimeBaseUrl + "mathjax-preview.html"

    visible: false
    title: i18nc("@title:window", "Formula")
    modality: Qt.NonModal
    flags: Qt.Tool | Qt.WindowTitleHint | Qt.WindowCloseButtonHint
    color: Kirigami.Theme.backgroundColor
    width: Kirigami.Units.gridUnit * 38
    height: Kirigami.Units.gridUnit * 28
    minimumWidth: Kirigami.Units.gridUnit * 24
    minimumHeight: Kirigami.Units.gridUnit * 16

    function prepare(details) {
        if (!readerActive || !details || (!details.mathml && !details.tex)) return;
        formula = details;
        nativeMenu.reset();
        requestSerial++;
        status = "loading";
        speechText = "";
        previewLoader.active = true;
        if (String(preview.url) !== previewUrl) preview.url = previewUrl;
        else if (!preview.loading) applyFormula();
    }

    function open(details) {
        if (!readerActive || !details || (!details.mathml && !details.tex)) return;
        cancelContextMenu();
        prepare(details);
        showPreparedFormula();
    }

    function showPreparedFormula() {
        if (!readerActive || !formula) return;
        cancelContextMenu();
        if (!positioned && transientParent) {
            x = transientParent.x + Math.max(0, (transientParent.width - width) / 2);
            y = transientParent.y + Kirigami.Units.gridUnit * 3;
            positioned = true;
        }
        show();
        requestActivate();
    }

    function openContextMenu(details, host) {
        if (!readerActive || !host || (!details?.mathml && !details?.tex)) return;
        cancelContextMenu();
        hide();
        prepare(details);
        contextMenuHost = host;
        contextMenuPending = true;
        host.prepareContextMenu();
    }

    function cancelContextMenu() {
        const host = contextMenuHost;
        contextMenuHost = null;
        contextMenuPending = false;
        menuActionPending = false;
        if (host) host.reset();
    }

    function cancelContextMenuIfIdle() {
        if (contextMenuHost && !contextMenuHost.active && !menuActionPending) cancelContextMenu();
    }

    function executeMenuAction(token, value, revision, request) {
        if (!preview) return;
        const action = ++menuActionSerial;
        menuActionPending = true;
        preview.runJavaScript("globalThis.formulaNativeAction?.(" + JSON.stringify(token) + ", "
            + JSON.stringify(value) + ", " + revision + ", " + request
            + ")?.then(() => {globalThis.formulaActionCompleted=" + action + "})");
    }

    function readCurrentFormula() {
        if (status === "ready" && speechText)
            readAloudRequested(Object.assign({}, formula?.speechSegment || {},
                {type: "math", text: speechText, lang: actualSpeechLocale, marks: []}));
    }

    function applyFormula() {
        if (formula && preview && String(preview.url) === previewUrl) {
            const details = Object.assign({}, formula, {locale: speechLocale, concise: conciseSpeech, rendering: renderingOptions});
            preview.runJavaScript("globalThis.showFormula?.(" + JSON.stringify(details) + ", " + requestSerial + ")");
        }
    }

    function updateSpeech() {
        if (!formula || synchronizingMenu) return;
        cancelContextMenu();
        nativeMenu.reset();
        requestSerial++;
        status = "loading";
        speechText = "";
        if (preview && !preview.loading) applyFormula();
    }

    onSpeechLocaleChanged: updateSpeech()
    onConciseSpeechChanged: updateSpeech()
    onRenderingOptionsChanged: if (visible) updateSpeech()

    function reset() {
        cancelContextMenu();
        nativeMenu.reset();
        close();
        requestSerial++;
        formula = null;
        speechText = "";
        actualSpeechLocale = "";
        previewLoader.active = false;
    }

    onReaderActiveChanged: if (!readerActive) reset()
    onVisibleChanged: if (!visible) nativeMenu.reset()

    // Menu preferences are isolated from the EPUB's WebEngine profile.
    WebEngineProfile {
        id: formulaProfile
        offTheRecord: true
        httpCacheType: WebEngineProfile.MemoryHttpCache
        persistentCookiesPolicy: WebEngineProfile.NoPersistentCookies
    }

    MathJaxMenu {
        id: nativeMenu
        anchors.fill: parent
        baseUrl: window.runtimeBaseUrl
        profile: formulaProfile
        onCopyRequested: text => Clipboard.savePlainText(text)
        onActionTriggered: (token, value, revision, request) => window.executeMenuAction(token, value, revision, request)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Kirigami.Units.smallSpacing

        RowLayout {
            Layout.fillWidth: true
            QQC2.Button {
                objectName: "mathJaxMenuButton"
                text: i18nc("@action:button", "MathJax Menu")
                enabled: window.status === "ready"
                onClicked: preview.runJavaScript("globalThis.openFormulaMenu?.()")
            }
            QQC2.Button {
                text: i18nc("@action:button", "Copy LaTeX")
                enabled: !!window.formula?.tex
                onClicked: Clipboard.savePlainText(window.formula.tex)
            }
            QQC2.Button {
                text: i18nc("@action:button", "Copy MathML")
                enabled: !!window.formula?.mathml
                onClicked: Clipboard.savePlainText(window.formula.mathml)
            }
            Item { Layout.fillWidth: true }
        }

        QQC2.Label {
            Layout.fillWidth: true
            text: i18nc("@info", "MathJax settings apply only to this formula preview.")
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            QQC2.Label { text: i18nc("@label:listbox", "Speech language") }
            QQC2.ComboBox {
                objectName: "mathSpeechLanguage"
                model: window.speechLanguages.map(code => ({code: code, name: Qt.locale(code).nativeLanguageName}))
                textRole: "name"
                currentIndex: window.speechLanguages.indexOf(window.speechLocale)
                onActivated: index => { window.speechLocale = window.speechLanguages[index]; }
            }
            QQC2.CheckBox {
                objectName: "mathConciseSpeech"
                text: i18nc("@option:check", "Concise speech")
                checked: window.conciseSpeech
                onToggled: window.conciseSpeech = checked
            }
            Item { Layout.fillWidth: true }
        }

        Loader {
            id: previewLoader
            Layout.fillWidth: true
            Layout.fillHeight: true
            active: false
            sourceComponent: WebEngineView {
                objectName: "mathJaxPreview"
                url: "about:blank"
                profile: formulaProfile
                settings.javascriptCanAccessClipboard: true
                onLoadingChanged: request => {
                    if (request.status === WebEngineView.LoadSucceededStatus) window.applyFormula();
                    else if (request.status === WebEngineView.LoadFailedStatus) window.status = "failed";
                }
                onNavigationRequested: request => {
                    if (request.isMainFrame && String(request.url) !== window.previewUrl && String(request.url) !== "about:blank") {
                        request.action = WebEngineView.IgnoreRequest;
                        if (/^https?:/.test(String(request.url))) Qt.openUrlExternally(request.url);
                    }
                }
                onNewWindowRequested: request => {
                    if (request.userInitiated && /^https?:/.test(String(request.requestedUrl))) Qt.openUrlExternally(request.requestedUrl);
                }
                onContextMenuRequested: request => { request.accepted = true; }
            }
        }

        QQC2.Label { text: i18nc("@label", "Speech description") }
        QQC2.ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 5
            QQC2.TextArea {
                objectName: "mathSpeechDescription"
                text: window.speechText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WordWrap
            }
        }
        RowLayout {
            QQC2.Button {
                objectName: "mathReadAloudButton"
                text: i18nc("@action:button", "Read Formula Aloud")
                enabled: window.status === "ready" && !!window.speechText
                onClicked: window.readCurrentFormula()
            }
            QQC2.Button {
                text: i18nc("@action:button", "Copy Speech Text")
                enabled: window.status === "ready" && !!window.speechText
                onClicked: Clipboard.savePlainText(window.speechText)
            }
        }

        QQC2.BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            running: window.visible && window.status === "loading"
            visible: running
        }
        QQC2.Label {
            Layout.fillWidth: true
            visible: window.status === "failed"
            text: i18nc("@info", "The formula preview could not be loaded.")
            wrapMode: Text.Wrap
        }
    }

    Timer {
        interval: 100
        repeat: true
        running: (window.visible || !!window.contextMenuHost) && !!preview && !preview.loading && !!window.formula
        onTriggered: {
            const serial = window.requestSerial;
            preview.runJavaScript("({status: globalThis.formulaStatus, request: globalThis.formulaRequest, speech: globalThis.formulaSpeech?.(), native: globalThis.formulaNativeState?.(), completed: globalThis.formulaActionCompleted})", value => {
                if (window.requestSerial === serial && value?.request === serial) {
                    window.status = value.status;
                    if (value.completed === window.menuActionSerial) window.menuActionPending = false;
                    if (value.status === "ready" || value.status === "failed") {
                        window.speechText = value.speech?.text || "";
                        window.actualSpeechLocale = value.speech?.locale || "";
                    }
                    // Reflect menu language changes without resetting its
                    // chosen speech rules or retypesetting a second time.
                    if (value.status === "ready" && value.native?.locale && window.speechLanguages.includes(value.native.locale)) {
                        window.synchronizingMenu = true;
                        window.speechLocale = value.native.locale;
                        const rules = value.native.speechRules || "";
                        window.conciseSpeech = rules.startsWith("clearspeak-") && rules.includes("Fraction_Over") && rules.includes("Ellipses_AndSoOn");
                        window.synchronizingMenu = false;
                    }
                    for (const event of value.native?.events || []) {
                        if (event.request === serial) {
                            if (window.contextMenuHost) window.contextMenuHost.handle(event);
                            else if (window.visible) nativeMenu.handle(event);
                        }
                    }
                    if (window.contextMenuPending && value.status === "ready") {
                        window.contextMenuPending = false;
                        preview.runJavaScript("globalThis.openFormulaMenu?.()");
                    } else if (window.contextMenuPending && value.status === "failed") {
                        window.contextMenuPending = false;
                        window.contextMenuHost.handle({kind: "error", request: serial, revision: window.contextMenuHost.revision,
                            text: i18nc("@info", "The formula preview could not be loaded.")});
                    }
                    window.cancelContextMenuIfIdle();
                }
            });
        }
    }
}
