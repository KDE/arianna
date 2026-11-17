// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import "reference-utils.mjs" as ReferenceUtils

ColumnLayout {
    id: root
    property var rows: []
    property string query: ""
    property string placeholderText: ""
    property string selectedValue: ""
    property string selectedLabel: ""
    property var matches: ReferenceUtils.matchingRows(rows, query)
    property alias searchField: search
    property alias listView: results
    signal selected(var row)
    spacing: Kirigami.Units.smallSpacing
    onMatchesChanged: results.currentIndex = -1

    Kirigami.SearchField {
        id: search
        objectName: root.objectName + "Search"
        Layout.fillWidth: true
        placeholderText: root.placeholderText
        text: root.query
        onTextChanged: if (root.query !== text) root.query = text
        Keys.onDownPressed: results.currentIndex = Math.min(results.count - 1, results.currentIndex + 1)
        Keys.onUpPressed: results.currentIndex = Math.max(0, results.currentIndex - 1)
        Keys.onReturnPressed: if (results.currentIndex >= 0) root.selected(root.matches.rows[results.currentIndex])
        Keys.onEnterPressed: if (results.currentIndex >= 0) root.selected(root.matches.rows[results.currentIndex])
    }
    QQC2.Label {
        Layout.fillWidth: true
        text: root.selectedLabel
        visible: text.length > 0
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        font.bold: true
    }
    QQC2.ScrollView {
        visible: root.enabled
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(4, Math.max(1, results.count)) * Kirigami.Units.gridUnit * 2.8
        clip: true
        ListView {
            id: results
            objectName: root.objectName + "Results"
            model: root.matches.rows
            currentIndex: -1
            boundsBehavior: Flickable.StopAtBounds
            delegate: ColumnLayout {
                id: rowDelegate
                required property var modelData
                required property int index
                readonly property bool startsGroup: !!modelData.group && (index === 0
                    || root.matches.rows[index - 1].group !== modelData.group)
                width: ListView.view.width
                height: implicitHeight
                spacing: Kirigami.Units.smallSpacing
                Kirigami.Separator {
                    objectName: root.objectName + "GroupSeparator"
                    Layout.fillWidth: true
                    visible: rowDelegate.startsGroup && rowDelegate.index > 0
                }
                QQC2.Label {
                    objectName: root.objectName + "GroupHeading"
                    Layout.fillWidth: true
                    visible: rowDelegate.startsGroup
                    text: rowDelegate.modelData.group || ""
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    font.bold: true
                    padding: Kirigami.Units.smallSpacing
                }
                QQC2.ItemDelegate {
                    readonly property string targetValue: rowDelegate.modelData.value
                    Layout.fillWidth: true
                    text: rowDelegate.modelData.label
                    highlighted: results.currentIndex === rowDelegate.index || targetValue === root.selectedValue
                    onClicked: root.selected(rowDelegate.modelData)
                    contentItem: QQC2.Label {
                        text: rowDelegate.modelData.label
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                    }
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.text: rowDelegate.modelData.tooltip || rowDelegate.modelData.label
                }
            }
        }
    }
    QQC2.Label {
        Layout.fillWidth: true
        visible: root.enabled
        text: root.matches.count === 0 ? i18n("No matches")
            : root.matches.count > 40 ? i18n("Showing 40 of %1 matches. Refine your search.", root.matches.count)
            : i18np("%1 match", "%1 matches", root.matches.count)
        wrapMode: Text.Wrap
    }
}
