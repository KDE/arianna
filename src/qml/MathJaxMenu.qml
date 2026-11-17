// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtWebEngine
import org.kde.kirigami as Kirigami

Item {
    id: root
    objectName: "mathJaxNativeControls"
    required property string baseUrl
    required property var profile
    property var currentMenu: null
    // Optional existing viewer menu/submenu. Standalone formula windows
    // continue to own their dynamically created menu.
    property var menuContainer: null
    property var submenuContainer: null
    property bool modelRequested: false
    readonly property bool active: !!currentMenu?.visible || info.visible || preferences.visible || numberDialog.visible || zoom.visible
    property int revision: 0
    property int request: 0
    property var dialogData: ({})
    signal actionTriggered(string token, var value, int revision, int request)
    signal copyRequested(string text)

    function label(text) {
        // MathJax 4 has no menu localization. Known labels are translated here;
        // language-specific SRE preference names remain the engine's own names.
        if (text.startsWith("Preferences for ")) return i18nc("@label", "Preferences for %1", text.slice(16));
        return translatedLabels[text] || text;
    }
    readonly property var translatedLabels: ({
        "Show Math As": i18nc("@action:inmenu MathJax", "Show Math As"),
        "Copy to Clipboard": i18nc("@action:inmenu MathJax", "Copy to Clipboard"),
        "Math Settings": i18nc("@action:inmenu MathJax", "Math Settings"),
        "MathML Code": i18nc("@action:inmenu MathJax", "MathML Code"),
        "Original MathML": i18nc("@action:inmenu MathJax", "Original MathML"),
        "TeX Commands": i18nc("@action:inmenu MathJax", "TeX Commands"),
        "Original Form": i18nc("@action:inmenu MathJax", "Original Form"),
        "Speech Text": i18nc("@action:inmenu MathJax", "Speech Text"),
        "Braille Code": i18nc("@action:inmenu MathJax", "Braille Code"),
        "SVG Image": i18nc("@action:inmenu MathJax", "SVG Image"),
        "Annotation": i18nc("@action:inmenu MathJax", "Annotation"),
        "Error Message": i18nc("@action:inmenu MathJax", "Error Message"),
        "Math Renderer": i18nc("@action:inmenu MathJax", "Math Renderer"),
        "Wide Expressions": i18nc("@action:inmenu MathJax", "Wide Expressions"),
        "Overflow": i18nc("@action:inmenu MathJax", "Overflow"),
        "Scroll": i18nc("@action:inmenu MathJax", "Scroll"),
        "Linebreak": i18nc("@action:inmenu MathJax", "Linebreak"),
        "Scale": i18nc("@action:inmenu MathJax", "Scale"),
        "Truncate": i18nc("@action:inmenu MathJax", "Truncate"),
        "Elide": i18nc("@action:inmenu MathJax", "Elide"),
        "Allow In-line Breaks": i18nc("@action:inmenu MathJax", "Allow In-line Breaks"),
        "MathML/SVG has": i18nc("@action:inmenu MathJax", "MathML/SVG has"),
        "Semantic attributes": i18nc("@action:inmenu MathJax", "Semantic attributes"),
        "LaTeX attributes": i18nc("@action:inmenu MathJax", "LaTeX attributes"),
        "TeX hints": i18nc("@action:inmenu MathJax", "TeX hints"),
        "Original as annotation": i18nc("@action:inmenu MathJax", "Original as annotation"),
        "Language": i18nc("@action:inmenu MathJax", "Language"),
        "Zoom Trigger": i18nc("@action:inmenu MathJax", "Zoom Trigger"),
        "Zoom Once Now": i18nc("@action:inmenu MathJax", "Zoom Once Now"),
        "Click": i18nc("@action:inmenu MathJax", "Click"),
        "Double-Click": i18nc("@action:inmenu MathJax", "Double-Click"),
        "No Zoom": i18nc("@action:inmenu MathJax", "No Zoom"),
        "Trigger Requires:": i18nc("@action:inmenu MathJax", "Trigger Requires:"),
        "Control": i18nc("@action:inmenu MathJax", "Control"),
        "Shift": i18nc("@action:inmenu MathJax", "Shift"),
        "Command": i18nc("@action:inmenu MathJax", "Command"),
        "Option": i18nc("@action:inmenu MathJax", "Option"),
        "Zoom Factor": i18nc("@action:inmenu MathJax", "Zoom Factor"),
        "Scale All Math...": i18nc("@action:inmenu MathJax", "Scale All Math..."),
        "Reset to defaults": i18nc("@action:inmenu MathJax", "Reset to defaults"),
        "Accessibility:": i18nc("@action:inmenu MathJax", "Accessibility:"),
        "Speech": i18nc("@action:inmenu MathJax", "Speech"),
        "Braille": i18nc("@action:inmenu MathJax", "Braille"),
        "Generate": i18nc("@action:inmenu MathJax", "Generate"),
        "Show Subtitles": i18nc("@action:inmenu MathJax", "Show Subtitles"),
        "Auto Voicing": i18nc("@action:inmenu MathJax", "Auto Voicing"),
        "Rules:": i18nc("@action:inmenu MathJax", "Rules:"),
        "Verbose": i18nc("@action:inmenu MathJax", "Verbose"),
        "Brief": i18nc("@action:inmenu MathJax", "Brief"),
        "Superbrief": i18nc("@action:inmenu MathJax", "Superbrief"),
        "Auto": i18nc("@action:inmenu MathJax", "Auto"),
        "No Preferences": i18nc("@action:inmenu MathJax", "No Preferences"),
        "Current Preferences": i18nc("@action:inmenu MathJax", "Current Preferences"),
        "Select Preferences": i18nc("@action:inmenu MathJax", "Select Preferences"),
        "Replace Speech": i18nc("@action:inmenu MathJax", "Replace Speech"),
        "Combine with Speech": i18nc("@action:inmenu MathJax", "Combine with Speech"),
        "Code Format:": i18nc("@action:inmenu MathJax", "Code Format:"),
        "Explorer": i18nc("@action:inmenu MathJax", "Explorer"),
        "Highlight": i18nc("@action:inmenu MathJax", "Highlight"),
        "Background": i18nc("@action:inmenu MathJax", "Background"),
        "Foreground": i18nc("@action:inmenu MathJax", "Foreground"),
        "Blue": i18nc("@action:inmenu MathJax", "Blue"),
        "Red": i18nc("@action:inmenu MathJax", "Red"),
        "Green": i18nc("@action:inmenu MathJax", "Green"),
        "Yellow": i18nc("@action:inmenu MathJax", "Yellow"),
        "White": i18nc("@action:inmenu MathJax", "White"),
        "Black": i18nc("@action:inmenu MathJax", "Black"),
        "None": i18nc("@action:inmenu MathJax", "None"),
        "Hover": i18nc("@action:inmenu MathJax", "Hover"),
        "Flame": i18nc("@action:inmenu MathJax", "Flame"),
        "Tree Coloring": i18nc("@action:inmenu MathJax", "Tree Coloring"),
        "Magnification": i18nc("@action:inmenu MathJax", "Magnification"),
        "Keyboard": i18nc("@action:inmenu MathJax", "Keyboard"),
        "Mouse": i18nc("@action:inmenu MathJax", "Mouse"),
        "Semantic Info": i18nc("@action:inmenu MathJax", "Semantic Info"),
        "Type": i18nc("@action:inmenu MathJax", "Type"),
        "Role": i18nc("@action:inmenu MathJax", "Role"),
        "Prefix": i18nc("@action:inmenu MathJax", "Prefix"),
        "Describe math as": i18nc("@action:inmenu MathJax", "Describe math as"),
        "MathJax expression": i18nc("@action:inmenu MathJax", "MathJax expression"),
        "math": i18nc("@action:inmenu MathJax", "math"),
        "clickable math": i18nc("@action:inmenu MathJax", "clickable math"),
        "explorable math": i18nc("@action:inmenu MathJax", "explorable math"),
        "none": i18nc("@action:inmenu MathJax", "none"),
        "Help message on focus": i18nc("@action:inmenu MathJax", "Help message on focus"),
        "Options": i18nc("@action:inmenu MathJax", "Options"),
        "Semantic Enrichment": i18nc("@action:inmenu MathJax", "Semantic Enrichment"),
        "Collapsible Math": i18nc("@action:inmenu MathJax", "Collapsible Math"),
        "Auto Collapse": i18nc("@action:inmenu MathJax", "Auto Collapse"),
        "Include in Tab Order": i18nc("@action:inmenu MathJax", "Include in Tab Order"),
        "Tabbing Focuses on": i18nc("@action:inmenu MathJax", "Tabbing Focuses on"),
        "Whole Expression": i18nc("@action:inmenu MathJax", "Whole Expression"),
        "Last Explored Node": i18nc("@action:inmenu MathJax", "Last Explored Node"),
        "Include Hidden MathML": i18nc("@action:inmenu MathJax", "Include Hidden MathML"),
        "About MathJax": i18nc("@action:inmenu MathJax", "About MathJax"),
        "MathJax Help": i18nc("@action:inmenu MathJax", "MathJax Help"),
        "MathJax MathML Expression": i18nc("@title:window", "MathJax MathML Expression"),
        "MathJax Original Source": i18nc("@title:window", "MathJax Original Source"),
        "MathJax Annotation Text": i18nc("@title:window", "MathJax Annotation Text"),
        "MathJax SVG Image": i18nc("@title:window", "MathJax SVG Image"),
        "MathJax Speech Text": i18nc("@title:window", "MathJax Speech Text"),
        "MathJax Braille Text": i18nc("@title:window", "MathJax Braille Text"),
        "MathJax Error Message": i18nc("@title:window", "MathJax Error Message"),
        "MathJax Zoomed Expression": i18nc("@title:window", "MathJax Zoomed Expression"),
        "ClearSpeak Preferences": i18nc("@title:window", "ClearSpeak Preferences"),
        "Scale All Math": i18nc("@title:window", "Scale All Math"),
        "Background opacity": i18nc("@label", "Background opacity"),
        "Foreground opacity": i18nc("@label", "Foreground opacity")
    })

    function fillMenu(menu, nodes) {
        for (const node of nodes) {
            if (node.type === "submenu") {
                const submenu = menuComponent.createObject(menu, {parent: root, title: label(node.text), objectName: "mathMenu_" + node.path.join("_")});
                fillMenu(submenu, node.children);
                menu.addMenu(submenu);
                menu.itemAt(menu.count - 1).enabled = node.enabled;
            } else if (node.type === "rule") {
                menu.addItem(separatorComponent.createObject(menu.contentItem));
            } else {
                const text = node.type === "slider" ? label(node.variable === "backgroundOpacity" ? "Background opacity" : "Foreground opacity") + "…" : label(node.text);
                menu.addItem(itemComponent.createObject(menu.contentItem, {
                    text: text, enabled: node.enabled && node.type !== "label",
                    checkable: node.type === "checkbox" || node.type === "radio",
                    checked: node.checked, token: node.token, menuRevision: revision, menuRequest: request,
                    objectName: "mathMenu_" + node.path.join("_")
                }));
            }
        }
    }

    function reset() {
        if (currentMenu) {
            currentMenu.close();
            if (currentMenu !== menuContainer) currentMenu.destroy();
            currentMenu = null;
        }
        modelRequested = false;
        if (submenuContainer) clearMenu(submenuContainer);
        info.close();
        preferences.close();
        numberDialog.close();
        zoom.close();
    }

    function clearMenu(menu) {
        while (menu.count) {
            if (menu.menuAt(0)) menu.removeMenu(menu.menuAt(0));
            else menu.removeItem(menu.itemAt(0));
        }
    }

    function prepareContextMenu() {
        reset();
        currentMenu = menuContainer;
        modelRequested = true;
        submenuContainer.addItem(itemComponent.createObject(submenuContainer.contentItem, {
            text: i18nc("@info", "Loading MathJax menu…"), enabled: false,
            token: "", menuRevision: 0, menuRequest: 0
        }));
    }

    function handle(event) {
        revision = event.revision;
        request = event.request;
        if (event.kind === "menu") {
            if (menuContainer) {
                if (!modelRequested) return;
                clearMenu(submenuContainer);
                fillMenu(submenuContainer, event.items);
                modelRequested = false;
                return;
            }
            reset();
            currentMenu = menuComponent.createObject(root, {objectName: "mathJaxNativeMenu", parent: root});
            fillMenu(currentMenu, event.items);
            currentMenu.popup(0, Kirigami.Units.gridUnit * 2);
        } else if (event.kind === "copy") {
            copyRequested(event.text);
        } else {
            dialogData = event;
            if (event.kind === "preferences") preferences.open();
            else if (event.kind === "slider") {
                percent.value = Math.round(event.value * 10);
                numberDialog.open();
            } else if (event.kind === "zoom") {
                zoom.ready = false;
                zoomLoader.active = true;
                zoomLoader.item.loadHtml(event.html, baseUrl);
                zoom.open();
            } else info.open();
        }
    }

    Component { id: menuComponent; QQC2.Menu { } }
    Component { id: separatorComponent; QQC2.MenuSeparator { } }
    Component {
        id: itemComponent
        QQC2.MenuItem {
            required property string token
            required property int menuRevision
            required property int menuRequest
            onTriggered: root.actionTriggered(token, null, menuRevision, menuRequest)
        }
    }

    QQC2.Dialog {
        id: info
        objectName: "mathJaxNativeInfo"
        parent: root
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        width: Math.min(root.width - Kirigami.Units.gridUnit * 2, Kirigami.Units.gridUnit * 34)
        height: Math.min(root.height - Kirigami.Units.gridUnit * 2, Kirigami.Units.gridUnit * 22)
        title: root.label(root.dialogData.title || "MathJax Error Message")
        modal: true
        standardButtons: QQC2.Dialog.Close
        ColumnLayout {
            anchors.fill: parent
            QQC2.ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                QQC2.TextArea {
                    objectName: "mathJaxNativeSource"
                    text: root.dialogData.text || ""
                    textFormat: TextEdit.PlainText
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.Wrap
                    font.family: root.dialogData.code ? "monospace" : Kirigami.Theme.defaultFont.family
                }
            }
            QQC2.Button {
                objectName: "mathJaxNativeCopy"
                text: root.label("Copy to Clipboard")
                onClicked: root.copyRequested(root.dialogData.text || "")
            }
            Repeater {
                model: root.dialogData.links || []
                QQC2.Button {
                    required property string modelData
                    text: modelData
                    onClicked: Qt.openUrlExternally(modelData)
                }
            }
        }
    }

    QQC2.Dialog {
        id: preferences
        objectName: "mathJaxNativePreferences"
        parent: root
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        width: Math.min(root.width - Kirigami.Units.gridUnit * 2, Kirigami.Units.gridUnit * 32)
        height: root.height - Kirigami.Units.gridUnit * 2
        title: root.label("ClearSpeak Preferences")
        modal: true
        standardButtons: QQC2.Dialog.Close
        QQC2.ScrollView {
            anchors.fill: parent
            contentWidth: availableWidth
            ColumnLayout {
                width: parent.width
                Repeater {
                    objectName: "mathJaxNativePreferenceRows"
                    model: root.dialogData.items || []
                    RowLayout {
                        id: preferenceRow
                        required property var modelData
                        QQC2.Label {
                            text: root.label(preferenceRow.modelData.label)
                            Layout.fillWidth: true
                        }
                        QQC2.ComboBox {
                            objectName: "mathPreference_" + preferenceRow.modelData.label
                            property var entry: preferenceRow.modelData
                            model: entry.choices.map(choice => root.label(choice))
                            currentIndex: entry.choices.indexOf(entry.value)
                            onActivated: index => root.actionTriggered(entry.token, entry.choices[index], root.dialogData.revision, root.dialogData.request)
                        }
                    }
                }
            }
        }
    }

    QQC2.Dialog {
        id: numberDialog
        objectName: "mathJaxNativeScale"
        parent: root
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        width: Math.min(root.width - Kirigami.Units.gridUnit * 2, Kirigami.Units.gridUnit * 24)
        title: root.label(root.dialogData.title || "Scale All Math")
        modal: true
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        onAccepted: root.actionTriggered(root.dialogData.token, percent.value / 10, root.dialogData.revision, root.dialogData.request)
        RowLayout {
            anchors.fill: parent
            QQC2.Slider {
                Layout.fillWidth: true
                from: root.dialogData.min || 0
                to: root.dialogData.max || 100
                value: percent.value / 10
                stepSize: 0.1
                onMoved: percent.value = Math.round(value * 10)
            }
            QQC2.SpinBox {
                id: percent
                objectName: "mathJaxNativePercent"
                editable: true
                from: Math.round((root.dialogData.min || 0) * 10)
                to: Math.round((root.dialogData.max || 100) * 10)
                textFromValue: (value, locale) => Number(value / 10).toLocaleString(locale, "f", 1) + " %"
                valueFromText: (text, locale) => Math.round(Number.fromLocaleString(locale, text.replace("%", "").trim()) * 10)
                validator: DoubleValidator { bottom: percent.from / 10; top: percent.to / 10; decimals: 1 }
            }
        }
    }

    QQC2.Dialog {
        id: zoom
        objectName: "mathJaxNativeZoom"
        property bool ready: false
        parent: root
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        width: root.width - Kirigami.Units.gridUnit * 2
        height: root.height * 0.7
        title: root.label("MathJax Zoomed Expression")
        modal: true
        standardButtons: QQC2.Dialog.Close
        onClosed: {
            ready = false;
            zoomLoader.active = false;
        }
        Loader {
            id: zoomLoader
            anchors.fill: parent
            active: false
            sourceComponent: WebEngineView {
                objectName: "mathJaxNativeZoomView"
                profile: root.profile
                settings.javascriptEnabled: false
                onLoadingChanged: request => { if (request.status === WebEngineView.LoadSucceededStatus) zoom.ready = true; }
                onContextMenuRequested: request => { request.accepted = true; }
                onNavigationRequested: request => { if (request.navigationType === WebEngineView.LinkClickedNavigation) request.action = WebEngineView.IgnoreRequest; }
            }
        }
    }
}
