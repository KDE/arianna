// SPDX-License-Identifier: GPL-2.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

Window {
    id: controls

    required property list<QQC2.Action> controlActions
    required property QQC2.Action rateDecreaseAction
    required property QQC2.Action rateIncreaseAction
    required property int speechRate
    required property string actionPrefix
    property bool readerActive: true
    property bool positioned: false
    readonly property int controlWidth: Math.ceil(buttons.implicitWidth + Kirigami.Units.smallSpacing * 2)
    readonly property int controlHeight: Math.ceil(buttons.implicitHeight + Kirigami.Units.smallSpacing * 2)

    signal stopRequested()

    visible: false
    title: i18nc("@title:window", "Read Aloud")
    modality: Qt.NonModal
    flags: Qt.Tool | Qt.WindowTitleHint | Qt.WindowCloseButtonHint
    color: Kirigami.Theme.backgroundColor
    width: controlWidth
    height: controlHeight
    minimumWidth: controlWidth
    maximumWidth: controlWidth
    minimumHeight: controlHeight
    maximumHeight: controlHeight

    function open() {
        if (!readerActive) return;
        if (!positioned && transientParent) {
            x = transientParent.x + Math.max(0, (transientParent.width - width) / 2);
            y = transientParent.y + Kirigami.Units.gridUnit * 3;
            positioned = true;
        }
        show();
        requestActivate();
    }

    onReaderActiveChanged: if (!readerActive) close()
    onClosing: stopRequested()

    RowLayout {
        id: buttons
        anchors.fill: parent
        anchors.margins: Kirigami.Units.smallSpacing
        spacing: Kirigami.Units.smallSpacing

        Repeater {
            model: controls.controlActions
            QQC2.ToolButton {
                required property var modelData
                action: modelData
                display: QQC2.AbstractButton.IconOnly
                QQC2.ToolTip.text: action.text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }

        QQC2.ToolSeparator {}

        QQC2.Label {
            id: rateLabel
            objectName: controls.actionPrefix + "SpeechRateLabel"
            text: i18nc("@label Speech rate correction in percent", "%1%", (controls.speechRate > 0 ? "+" : "") + controls.speechRate)
            horizontalAlignment: Text.AlignRight
            Layout.minimumWidth: rateMetrics.width
            Accessible.name: i18nc("@info:tooltip", "Speech rate correction: %1", text)

            TextMetrics {
                id: rateMetrics
                font: rateLabel.font
                text: "+100%"
            }
        }

        ColumnLayout {
            spacing: 0

            Repeater {
                model: [controls.rateIncreaseAction, controls.rateDecreaseAction]
                QQC2.ToolButton {
                    required property var modelData
                    action: modelData
                    display: QQC2.AbstractButton.IconOnly
                    icon.width: Kirigami.Units.iconSizes.small
                    icon.height: Kirigami.Units.iconSizes.small
                    implicitWidth: Kirigami.Units.iconSizes.small + Kirigami.Units.smallSpacing * 2
                    implicitHeight: Kirigami.Units.iconSizes.small + Kirigami.Units.smallSpacing
                    QQC2.ToolTip.text: action.text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }
            }
        }
    }
}
