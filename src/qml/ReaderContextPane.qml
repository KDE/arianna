// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.arianna

ColumnLayout {
    id: root
    property var snapshot: ({})
    property string editedText: ""
    property var excludedFields: ["surrounding.section"]
    property bool canRefresh: false
    property var submissions: []
    property bool expanded: false
    readonly property var context: ReaderContextBuilder.withOptions(snapshot, excludedFields, editedText)
    readonly property string contextJson: ReaderContextBuilder.toJson(context)
    signal refreshRequested()

    function optionalFields() {
        const fields = [
            ["bibliography.authors", i18n("Authors")], ["bibliography.translators", i18n("Translators")],
            ["bibliography.editors", i18n("Editors")], ["bibliography.languages", i18n("Book languages")],
            ["bibliography.edition", i18n("Edition")], ["location.chapter", i18n("Chapter")],
            ["location.chapterHref", i18n("Chapter link")], ["location.heading", i18n("Chapter heading")],
            ["location.verse", i18n("Verse")], ["location.selectionCfi", i18n("Selection CFI")],
            ["location.readerCfi", i18n("Reader CFI")], ["location.sectionIndex", i18n("Section index")],
            ["selection.language", i18n("Selection language")], ["selection.languageSegments", i18n("Language segments")],
            ["surrounding.sentence", i18n("Surrounding sentence")], ["surrounding.paragraph", i18n("Surrounding paragraph")],
            ["surrounding.section", i18n("Larger section")]
        ];
        return fields.filter(field => {
            const path = field[0].split(".");
            return snapshot[path[0]] && snapshot[path[0]][path[1]] !== undefined;
        });
    }

    function setIncluded(path, included) {
        const fields = excludedFields.filter(field => field !== path);
        if (!included) fields.push(path);
        excludedFields = fields;
    }

    RowLayout {
        Layout.fillWidth: true
        QQC2.ToolButton {
            objectName: "readerContextExpandButton"
            text: i18n("Context")
            icon.name: root.expanded ? "arrow-down" : "arrow-right"
            checkable: true
            checked: root.expanded
            onClicked: root.expanded = !root.expanded
        }
        Item { Layout.fillWidth: true }
        QQC2.Button {
            objectName: "readerContextRefreshButton"
            text: i18n("Update context")
            icon.name: "view-refresh"
            enabled: root.canRefresh && !CodexClient.busy
            onClicked: root.refreshRequested()
        }
    }

    QQC2.Label {
        objectName: "readerContextSummary"
        Layout.fillWidth: true
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        maximumLineCount: 3
        elide: Text.ElideRight
        text: {
            const b = root.context.bibliography || {};
            const l = root.context.location || {};
            const s = root.context.selection || {};
            const names = [].concat(b.authors || [], b.translators || [], b.editors || []).join(", ");
            return [b.title, names, l.chapter || l.heading, l.verse, s.language, s.text].filter(value => value).join(" · ");
        }
    }

    ColumnLayout {
        visible: root.expanded
        Layout.fillWidth: true
        QQC2.Label {
            Layout.fillWidth: true
            text: i18n("Choose context for the next question. Previously sent context remains part of the conversation.")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font: Kirigami.Theme.smallFont
        }
        QQC2.ScrollView {
            id: contextOptionScroll
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contextOptions.implicitHeight, Kirigami.Units.gridUnit * 3)
            clip: true
            Flow {
                id: contextOptions
                width: contextOptionScroll.availableWidth
                Repeater {
                    model: root.optionalFields()
                    QQC2.CheckBox {
                        required property var modelData
                        objectName: "readerContextOption_" + modelData[0]
                        text: modelData[1]
                        checked: root.excludedFields.indexOf(modelData[0]) < 0
                        onClicked: root.setIncluded(modelData[0], checked)
                    }
                }
            }
        }
        QQC2.ComboBox {
            id: contextHistory
            objectName: "readerContextHistory"
            Layout.fillWidth: true
            model: [i18n("Context for next question")].concat(root.submissions.map((entry, index) =>
                i18n("Sent question %1 — %2", index + 1, entry.status)))
        }
        QQC2.ScrollView {
            id: inspectorScroll
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 6
            clip: true
            QQC2.TextArea {
                objectName: "readerContextJson"
                width: inspectorScroll.availableWidth
                textFormat: TextEdit.PlainText
                readOnly: true
                selectByMouse: true
                wrapMode: Text.Wrap
                text: contextHistory.currentIndex <= 0 ? root.contextJson
                    : ReaderContextBuilder.toJson(root.submissions[contextHistory.currentIndex - 1] || {})
            }
        }
    }
}
