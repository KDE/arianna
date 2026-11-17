// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: root
    property string selectedColor: "yellow"

    QQC2.Label { text: i18nc("@label", "Highlight color") }
    GridLayout {
        columns: 8
        rowSpacing: Kirigami.Units.smallSpacing
        columnSpacing: Kirigami.Units.smallSpacing
        Repeater {
            model: [
                {value: "#ffff00", name: i18nc("@item:inlistbox Highlight color", "Yellow")},
                {value: "#ffb74d", name: i18nc("@item:inlistbox Highlight color", "Orange")},
                {value: "#ef5350", name: i18nc("@item:inlistbox Highlight color", "Red")},
                {value: "#f06292", name: i18nc("@item:inlistbox Highlight color", "Pink")},
                {value: "#ba68c8", name: i18nc("@item:inlistbox Highlight color", "Purple")},
                {value: "#64b5f6", name: i18nc("@item:inlistbox Highlight color", "Blue")},
                {value: "#4dd0e1", name: i18nc("@item:inlistbox Highlight color", "Turquoise")},
                {value: "#81c784", name: i18nc("@item:inlistbox Highlight color", "Green")},
                {value: "#fff9c4", name: i18nc("@item:inlistbox Highlight color", "Light yellow")},
                {value: "#ffe0b2", name: i18nc("@item:inlistbox Highlight color", "Light orange")},
                {value: "#ffcdd2", name: i18nc("@item:inlistbox Highlight color", "Light red")},
                {value: "#f8bbd0", name: i18nc("@item:inlistbox Highlight color", "Light pink")},
                {value: "#e1bee7", name: i18nc("@item:inlistbox Highlight color", "Light purple")},
                {value: "#bbdefb", name: i18nc("@item:inlistbox Highlight color", "Light blue")},
                {value: "#b2ebf2", name: i18nc("@item:inlistbox Highlight color", "Light turquoise")},
                {value: "#c8e6c9", name: i18nc("@item:inlistbox Highlight color", "Light green")}
            ]
            delegate: QQC2.AbstractButton {
                id: swatch
                required property var modelData
                required property int index
                objectName: "annotationColorSwatch" + index
                implicitWidth: Kirigami.Units.gridUnit * 1.6
                implicitHeight: implicitWidth
                text: modelData.name
                checked: Qt.colorEqual(root.selectedColor, modelData.value)
                Accessible.role: Accessible.RadioButton
                Accessible.name: text
                Accessible.checkable: true
                Accessible.checked: checked
                onClicked: root.selectedColor = modelData.value
                background: Rectangle {
                    color: swatch.modelData.value
                    radius: Kirigami.Units.cornerRadius
                    border.width: swatch.checked || swatch.activeFocus ? 3 : 1
                    border.color: swatch.activeFocus ? Kirigami.Theme.focusColor : Kirigami.Theme.textColor
                }
                contentItem: QQC2.Label {
                    text: swatch.checked ? "✓" : ""
                    color: "#202020"
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }
    }
}
