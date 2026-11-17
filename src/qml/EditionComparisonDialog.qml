// SPDX-License-Identifier: GPL-2.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.arianna as Arianna

QQC2.Dialog {
    id: root
    objectName: "editionComparisonDialog"
    property var comparison: ({})
    property var hint: null
    readonly property var entries: hint ? [hint] : (comparison.documents || [])
    readonly property var entry: entries[documentPicker.currentIndex] || ({})
    modal: true
    title: i18n("Compare Import with Existing Edition")
    standardButtons: QQC2.Dialog.Close
    anchors.centerIn: parent
    width: Math.min(Kirigami.Units.gridUnit * 70, parent ? parent.width - Kirigami.Units.gridUnit * 2 : 1200)
    height: Math.min(Kirigami.Units.gridUnit * 48, parent ? parent.height - Kirigami.Units.gridUnit * 2 : 800)

    function showComparison(data, anchorHint = null) {
        comparison = data;
        hint = anchorHint;
        documentPicker.currentIndex = 0;
        modePicker.currentIndex = 0;
        open();
    }

    contentItem: ColumnLayout {
        QQC2.Label {
            Layout.fillWidth: true
            text: root.comparison.message || ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
        QQC2.Label {
            Layout.fillWidth: true
            text: i18n("XML is formatted for comparison. Anchor locations refer to lines in the original files. Diff: − import, + existing edition.")
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            QQC2.ComboBox {
                id: documentPicker
                objectName: "comparisonDocument"
                Layout.fillWidth: true
                model: root.entries
                textRole: "label"
            }
            QQC2.ComboBox {
                id: modePicker
                objectName: "comparisonMode"
                model: [i18n("Side by Side"), i18n("Diff (Import → Existing)")]
            }
        }
        QQC2.Label {
            Layout.fillWidth: true
            visible: !!root.entry.message || !!root.entry.sourceTruncated || !!root.comparison.limited
            text: (root.entry.message || "")
                + ((root.entry.sourceTruncated || root.comparison.limited) ? "\n" + i18n("The comparison output is limited. Large passages may be shortened and additional documents omitted.") : "")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
        RowLayout {
            visible: modePicker.currentIndex === 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            Repeater {
                model: ["import", "existing"]
                delegate: ColumnLayout {
                    required property string modelData
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 1
                    QQC2.Label {
                        Layout.fillWidth: true
                        text: modelData === "import" ? i18n("Import") : i18n("Existing Edition")
                        font.bold: true
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        text: (root.comparison[modelData + "File"] || "") + "\n" + (root.entry[modelData + "Document"] || i18n("No matching document"))
                            + (root.entry[modelData + "Line"] > 0 ? " · " + i18n("Line: %1", root.entry[modelData + "Line"]) : "")
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                    }
                    QQC2.ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        QQC2.TextArea {
                            objectName: modelData + "ComparisonSource"
                            text: root.entry[modelData + "Source"] || ""
                            textFormat: TextEdit.PlainText
                            readOnly: true
                            selectByMouse: true
                            font.family: "monospace"
                            wrapMode: TextEdit.NoWrap
                        }
                    }
                }
            }
        }
        QQC2.ScrollView {
            visible: modePicker.currentIndex === 1
            Layout.fillWidth: true
            Layout.fillHeight: true
            QQC2.TextArea {
                objectName: "editionUnifiedDiff"
                text: root.entry.diff || (root.entry.importSource === root.entry.existingSource
                    ? i18n("No differences in this passage.") : i18n("No complete diff is available for this passage. Use the side-by-side comparison."))
                textFormat: TextEdit.PlainText
                readOnly: true
                selectByMouse: true
                font.family: "monospace"
                wrapMode: TextEdit.NoWrap
            }
        }
        QQC2.Label {
            Layout.fillWidth: true
            visible: root.entries.length === 0
            text: i18n("No changed source documents found.")
        }
        RowLayout {
            QQC2.Button {
                objectName: "copyExistingAnchor"
                visible: !!root.entry.anchorHtml
                text: i18n("Copy Original Anchor HTML")
                icon.name: "edit-copy"
                onClicked: Arianna.Clipboard.savePlainText(root.entry.anchorHtml)
            }
            QQC2.Button {
                objectName: "copyEditionDiff"
                text: i18n("Copy Diff")
                icon.name: "edit-copy"
                enabled: !!root.entry.diff
                onClicked: Arianna.Clipboard.savePlainText(root.entry.diff)
            }
        }
    }
}
