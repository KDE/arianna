// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.arianna
import "reference-utils.mjs" as ReferenceUtils

ColumnLayout {
    id: root
    property var rows: []
    property string query: ""
    property string placeholderText: ""
    property string selectedValue: ""
    property string selectedLabel: ""
    property alias searchField: search
    property alias treeView: results
    property alias targetModel: targets
    readonly property int matchCount: targets.matchCount
    property var expandedKeys: ({})
    property bool restoring: false
    property int keyboardRow: -1
    signal selected(var row)
    spacing: Kirigami.Units.smallSpacing

    onRowsChanged: expandedKeys = ({})
    onSelectedValueChanged: if (!query.trim()) Qt.callLater(revealSelection)

    function chooseAt(row) {
        if (row < 0 || row >= results.rows) return;
        const index = results.index(row, 0);
        results.selectionModel.setCurrentIndex(index, ItemSelectionModel.NoUpdate);
        const value = targets.data(index, ReferenceTargetModel.TargetValueRole);
        if (value) selected(targets.data(index, ReferenceTargetModel.TargetDataRole));
        else results.toggleExpanded(row);
    }
    function moveCurrent(offset) {
        keyboardRow = Math.max(0, Math.min(results.rows - 1, keyboardRow + offset));
        if (results.rows) results.positionViewAtRow(keyboardRow, TableView.Contain);
    }
    function remember(row, expanded) {
        if (restoring || query.trim()) return;
        const key = targets.keyForIndex(results.index(row, 0));
        if (key) expandedKeys[key] = expanded;
    }
    function restoreBranch(parentIndex, depth) {
        for (let i = 0; i < targets.rowCount(parentIndex); ++i) {
            const index = targets.index(i, 0, parentIndex);
            if (!targets.rowCount(index)) continue;
            const key = targets.keyForIndex(index);
            const expand = expandedKeys[key] ?? (depth === 0);
            if (expand) {
                results.expand(results.rowAtIndex(index));
                restoreBranch(index, depth + 1);
            }
        }
    }
    function restoreExpansion() {
        restoring = true;
        keyboardRow = -1;
        if (query.trim()) results.expandRecursively();
        else {
            results.collapseRecursively();
            restoreBranch(targets.index(-1, -1), 0);
            revealSelection();
        }
        restoring = false;
    }
    function revealSelection() {
        if (query.trim() || !selectedValue) return;
        const index = targets.indexForValue(selectedValue);
        if (!index.valid) return;
        const wasRestoring = restoring;
        restoring = true;
        results.expandToIndex(index);
        results.positionViewAtRow(results.rowAtIndex(index), TableView.Contain);
        restoring = wasRestoring;
    }

    ReferenceTargetModel {
        id: targets
        rows: root.rows
        filterText: ReferenceUtils.referenceSearchKey(root.query)
        onModelReset: Qt.callLater(root.restoreExpansion)
    }
    Kirigami.SearchField {
        id: search
        objectName: root.objectName + "Search"
        Layout.fillWidth: true
        placeholderText: root.placeholderText
        text: root.query
        onTextChanged: if (root.query !== text) root.query = text
        Keys.onDownPressed: root.moveCurrent(1)
        Keys.onUpPressed: root.moveCurrent(-1)
        Keys.onRightPressed: if (root.keyboardRow >= 0) results.expand(root.keyboardRow)
        Keys.onLeftPressed: if (root.keyboardRow >= 0) results.collapse(root.keyboardRow)
        Keys.onReturnPressed: root.chooseAt(root.keyboardRow)
        Keys.onEnterPressed: root.chooseAt(root.keyboardRow)
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
        Layout.preferredHeight: Kirigami.Units.gridUnit * 12
        clip: true
        TreeView {
            id: results
            objectName: root.objectName + "Results"
            model: targets
            selectionModel: ItemSelectionModel { model: targets }
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            columnWidthProvider: column => width
            selectionBehavior: TableView.SelectRows
            editTriggers: TableView.NoEditTriggers
            onCurrentRowChanged: root.keyboardRow = currentRow
            Keys.onReturnPressed: root.chooseAt(currentRow)
            Keys.onEnterPressed: root.chooseAt(currentRow)
            onExpanded: (row, depth) => root.remember(row, true)
            onCollapsed: (row, recursively) => root.remember(row, false)
            delegate: QQC2.TreeViewDelegate {
                id: targetDelegate
                required property string label
                required property string targetValue
                required property string tooltip
                implicitHeight: Kirigami.Units.gridUnit * 2.5
                highlighted: (!!targetValue && targetValue === root.selectedValue) || row === root.keyboardRow
                onClicked: root.chooseAt(row)
                contentItem: QQC2.Label {
                    text: targetDelegate.label
                    font.bold: targetDelegate.hasChildren
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    color: targetDelegate.highlighted ? targetDelegate.palette.highlightedText : targetDelegate.palette.buttonText
                }
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.text: tooltip
            }
        }
    }
    QQC2.Label {
        Layout.fillWidth: true
        visible: root.enabled
        text: root.matchCount === 0 ? i18n("No matches") : i18np("%1 match", "%1 matches", root.matchCount)
        wrapMode: Text.Wrap
    }
}
