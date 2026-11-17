// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import org.kde.arianna

FormCard.AbstractFormDelegate {
    id: root
    objectName: "mathJaxDisplaySettings"
    enabled: Config.mathJaxSupport
    signal readAloudRequested(var segment)
    onEnabledChanged: if (!enabled && preview) preview.item?.reset()
    background: Item {}
    focusPolicy: Qt.NoFocus
    readonly property var renderingOptions: ({scale: Config.mathJaxScale / 100,
        inlineScale: Config.mathJaxInlineScale / 100,
        align: Config.mathJaxAlignment, overflow: Config.mathJaxOverflow,
        inline: Config.mathJaxBreakInline, fontCache: Config.mathJaxFontCache})

    function openPreview(display) {
        preview.active = true;
        preview.item.open({tex: "1+\\frac{1}{2^2}+\\frac{1}{3^2}+\\frac{1}{4^2}+\\cdots=\\frac{\\pi^2}{6}", display: display});
    }

    Loader {
        id: preview
        active: false
        sourceComponent: MathJaxWindow {
            runtimeBaseUrl: bookServerBaseUrl + "/mathjax/" + encodeURIComponent(bookServerSessionToken) + "/"
            renderingOptions: root.renderingOptions
            transientParent: applicationWindow()
            onReadAloudRequested: segment => root.readAloudRequested(segment)
        }
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing
        QQC2.Label {
            text: i18nc("@title:group", "Mathematics display")
            font.bold: true
        }
        RowLayout {
            Layout.fillWidth: true
            QQC2.Label {
                id: sizeLabel
                text: i18nc("@label:slider", "Display formula size")
                Layout.fillWidth: true
            }
            QQC2.Label { text: i18nc("@label", "%1 %", Math.round(scale.value)) }
        }
        QQC2.Slider {
            id: scale
            objectName: "mathJaxScaleSlider"
            Layout.fillWidth: true
            Accessible.name: sizeLabel.text
            from: 50
            to: 200
            stepSize: 5
            value: Config.mathJaxScale
            onMoved: { Config.mathJaxScale = Math.round(value); Config.save(); }
        }
        RowLayout {
            Layout.fillWidth: true
            QQC2.Label {
                id: inlineSizeLabel
                text: i18nc("@label:slider", "Inline formula size")
                Layout.fillWidth: true
            }
            QQC2.Label { text: i18nc("@label", "%1 %", Math.round(inlineScale.value)) }
        }
        QQC2.Slider {
            id: inlineScale
            objectName: "mathJaxInlineScaleSlider"
            Layout.fillWidth: true
            Accessible.name: inlineSizeLabel.text
            from: 50
            to: 200
            stepSize: 5
            value: Config.mathJaxInlineScale
            onMoved: { Config.mathJaxInlineScale = Math.round(value); Config.save(); }
        }
        QQC2.Label { text: i18nc("@label", "Formula alignment") }
        QQC2.ButtonGroup { id: alignment }
        RowLayout {
            Layout.fillWidth: true
            Repeater {
                model: [
                    {value: "left", label: i18nc("@option:check formula alignment", "Left")},
                    {value: "center", label: i18nc("@option:check formula alignment", "Centered")},
                    {value: "right", label: i18nc("@option:check formula alignment", "Right")}
                ]
                QQC2.Button {
                    required property var modelData
                    objectName: "mathJaxAlignment_" + modelData.value
                    Layout.fillWidth: true
                    text: modelData.label
                    checkable: true
                    checked: Config.mathJaxAlignment === modelData.value
                    QQC2.ButtonGroup.group: alignment
                    onClicked: { Config.mathJaxAlignment = modelData.value; Config.save(); }
                }
            }
        }
        QQC2.Label { text: i18nc("@label:listbox", "Wide equations") }
        QQC2.ComboBox {
            objectName: "mathJaxOverflowChoice"
            Layout.fillWidth: true
            textRole: "label"
            valueRole: "value"
            Accessible.name: i18nc("@label:listbox", "Wide equations")
            model: [
                {value: "linebreak", label: i18nc("@item:inlistbox", "Automatic line breaks")},
                {value: "scale", label: i18nc("@item:inlistbox", "Shrink to fit")},
                {value: "scroll", label: i18nc("@item:inlistbox", "Horizontal scrolling")},
                {value: "overflow", label: i18nc("@item:inlistbox", "Allow overflow")}
            ]
            currentIndex: Math.max(0, model.findIndex(item => item.value === Config.mathJaxOverflow))
            onActivated: index => { Config.mathJaxOverflow = model[index].value; Config.save(); }
        }
        QQC2.CheckBox {
            objectName: "mathJaxBreakInlineSwitch"
            text: i18nc("@option:check", "Line breaks in inline formulas too")
            checked: Config.mathJaxBreakInline
            onToggled: { Config.mathJaxBreakInline = checked; Config.save(); }
        }
        QQC2.Label { text: i18nc("@label", "SVG font cache") }
        QQC2.ButtonGroup { id: cache }
        RowLayout {
            Layout.fillWidth: true
            Repeater {
                model: [
                    {value: "local", label: i18nc("@option:radio font cache", "Local")},
                    {value: "global", label: i18nc("@option:radio font cache", "Global")},
                    {value: "none", label: i18nc("@option:radio font cache", "None")}
                ]
                QQC2.RadioButton {
                    required property var modelData
                    objectName: "mathJaxFontCache_" + modelData.value
                    text: modelData.label
                    checked: Config.mathJaxFontCache === modelData.value
                    QQC2.ButtonGroup.group: cache
                    onClicked: { Config.mathJaxFontCache = modelData.value; Config.save(); }
                }
            }
        }
        RowLayout {
            QQC2.Button {
                objectName: "mathJaxSettingsPreview"
                text: i18nc("@action:button", "Display formula preview")
                onClicked: root.openPreview(true)
            }
            QQC2.Button {
                objectName: "mathJaxInlinePreview"
                text: i18nc("@action:button", "Inline formula preview")
                onClicked: root.openPreview(false)
            }
        }
    }
}
