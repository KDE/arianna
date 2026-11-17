// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.arianna

QQC2.Dialog {
    id: root
    title: i18n("Sanskrit Work Structure")
    modal: true
    closePolicy: QQC2.Popup.NoAutoClose
    width: Math.min(parent ? parent.width - 32 : 900, 1100)
    height: Math.min(parent ? parent.height - 32 : 700, 850)
    property var rows: []
    property var levels: []
    property var draft: ({})
    property bool dirty: false
    property int loadedDraftVersion: -1
    readonly property string validity: review.snapshot.existing?.validity || "absent"
    readonly property var choices: [{id: "", iast: i18n("Unassigned")}].concat(review.terms)
    signal structureSaved(string bookId)

    function showForBook(bookId) {
        open();
        review.analyze(bookId, bookServerBaseUrl, bookServerSessionToken);
    }
    function synchronize() {
        if (!review.snapshot.snapshotId) return;
        const current = review.draft;
        const assignments = new Map((current.assignments || []).map(a => [a.evidenceId, a]));
        root.draft = JSON.parse(JSON.stringify(current));
        root.levels = Array.from(current.levels || []);
        root.rows = (review.snapshot.evidence || []).map(e => {
            const a = assignments.get(e.id);
            return {
                evidenceId: e.id, label: e.label, kind: e.kind, href: e.href || e.cfi,
                term: a?.term || "", coordinateText: (a?.coordinate || e.numbers || []).join("."),
                origin: a?.origin || "manual", reason: a?.reason || "",
                uncertainties: a?.uncertainties || [], sources: a?.sources || [], confirmed: !!a?.confirmed
            };
        });
        root.loadedDraftVersion = review.draftVersion;
        root.dirty = false;
    }
    function editRow(index, key, value) {
        const list = root.rows.slice();
        const row = Object.assign({}, list[index]);
        row[key] = value;
        if (key !== "confirmed") {
            row.origin = "manual";
            row.confirmed = false;
        }
        list[index] = row;
        root.rows = list;
        root.dirty = true;
    }
    function setLevels(value) {
        root.levels = value;
        root.rows = root.rows.map(row => Object.assign({}, row, {confirmed: false}));
        root.dirty = true;
    }
    function coordinateNumber(part) {
        const text = part.trim().replace(/[०-९]/g, c => String(c.charCodeAt(0) - 0x0966));
        return /^\d+$/.test(text) ? Number(text) : -1;
    }
    function submittedDraft() {
        return Object.assign({}, root.draft, {
            levels: root.levels,
            separators: [periodSeparator.checked ? "." : "", hyphenSeparator.checked ? "-" : "", colonSeparator.checked ? ":" : ""].filter(Boolean),
            assignments: root.rows.filter(row => row.term && row.coordinateText && row.confirmed).map(row => ({
                evidenceId: row.evidenceId, term: row.term,
                coordinate: row.coordinateText.split(/[.:-]/).map(part => root.coordinateNumber(part)),
                origin: row.origin, reason: row.reason, uncertainties: row.uncertainties,
                sources: row.sources, confirmed: true
            }))
        });
    }
    WorkStructureReview {
        id: review
        onChanged: if (!busy && snapshot.snapshotId && root.loadedDraftVersion !== draftVersion) root.synchronize()
        onSaved: bookId => root.structureSaved(bookId)
    }
    onClosed: { review.cancel(); root.draft = ({}); root.rows = []; }
    contentItem: ColumnLayout {
        spacing: 8
        QQC2.Label {
            Layout.fillWidth: true
            text: i18n("Check each mapping against the EPUB evidence. TOC labels and source text remain unchanged. Writing is an explicit editorial action.")
            wrapMode: Text.Wrap
        }
        QQC2.Label {
            Layout.fillWidth: true
            text: review.snapshot.bibliography?.title || ""
            font.bold: true
            textFormat: Text.PlainText
        }
        QQC2.Label {
            Layout.fillWidth: true
            visible: root.validity !== "absent"
            text: root.validity === "valid" ? i18n("A confirmed structure is already present. Automatic analysis will not replace it.")
                : i18n("The existing structure is preserved but requires review: %1", root.validity)
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
        }
        RowLayout {
            Layout.fillWidth: true
            QQC2.Label { text: i18n("Levels:") }
            Repeater {
                model: root.levels
                RowLayout {
                    required property string modelData
                    required property int index
                    QQC2.Label { text: review.terms.find(t => t.id === modelData)?.iast || modelData }
                    QQC2.ToolButton {
                        text: "←"
                        Accessible.name: i18n("Move level up")
                        enabled: index > 0 && !review.busy
                        onClicked: {
                            const levels = root.levels.slice();
                            [levels[index - 1], levels[index]] = [levels[index], levels[index - 1]];
                            root.setLevels(levels);
                        }
                    }
                    QQC2.ToolButton {
                        text: "×"
                        Accessible.name: i18n("Remove level")
                        enabled: !review.busy
                        onClicked: root.setLevels(root.levels.filter((term, i) => i !== index))
                    }
                }
            }
            QQC2.ComboBox { id: newLevel; model: review.terms; textRole: "iast"; valueRole: "id"; enabled: !review.busy }
            QQC2.Button {
                text: i18n("Add level")
                enabled: !review.busy && !root.levels.includes(newLevel.currentValue)
                onClicked: root.setLevels(root.levels.concat([newLevel.currentValue]))
            }
        }
        QQC2.Label { Layout.fillWidth: true; text: i18n("EPUB evidence → structure term and coordinate → confirmation"); wrapMode: Text.Wrap }
        ListView {
            id: mappings
            objectName: "workStructureMappings"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 150
            clip: true
            model: root.rows
            QQC2.ScrollBar.vertical: QQC2.ScrollBar {}
            delegate: ColumnLayout {
                required property var modelData
                required property int index
                width: mappings.width - 18
                spacing: 2
                QQC2.Label {
                    Layout.fillWidth: true
                    text: modelData.label
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    text: modelData.href
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    font.pointSize: 8
                }
                RowLayout {
                    QQC2.ComboBox {
                        model: root.choices
                        textRole: "iast"
                        valueRole: "id"
                        currentIndex: root.choices.findIndex(t => t.id === modelData.term)
                        enabled: !review.busy
                        onActivated: root.editRow(index, "term", currentValue)
                    }
                    QQC2.TextField {
                        text: modelData.coordinateText
                        placeholderText: i18n("Coordinate, e.g. 1.4")
                        Layout.preferredWidth: 130
                        enabled: !review.busy
                        onEditingFinished: if (text !== modelData.coordinateText) root.editRow(index, "coordinateText", text)
                    }
                    QQC2.CheckBox {
                        text: i18n("Confirmed")
                        checked: modelData.confirmed
                        enabled: !review.busy && !!modelData.term && root.levels.includes(modelData.term) && !!modelData.coordinateText
                        onClicked: root.editRow(index, "confirmed", checked)
                    }
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    text: [modelData.reason, ...modelData.uncertainties, ...modelData.sources.map(s => i18n("Unverified source hint: %1", s))].filter(Boolean).join("\n")
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.pointSize: 9
                }
                Kirigami.Separator { Layout.fillWidth: true }
            }
        }
        RowLayout {
            QQC2.Label { text: i18n("Accepted coordinate separators:") }
            QQC2.CheckBox { id: periodSeparator; text: "."; checked: (root.draft.separators || []).includes("."); enabled: !review.busy; onClicked: root.dirty = true }
            QQC2.CheckBox { id: hyphenSeparator; text: "-"; checked: (root.draft.separators || []).includes("-"); enabled: !review.busy; onClicked: root.dirty = true }
            QQC2.CheckBox { id: colonSeparator; text: ":"; checked: (root.draft.separators || []).includes(":"); enabled: !review.busy; onClicked: root.dirty = true }
        }
        QQC2.Label {
            Layout.fillWidth: true
            text: review.message + ((review.snapshot.issues || []).length ? "\n" + review.snapshot.issues.join("\n") : "")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
        QQC2.Label {
            Layout.fillWidth: true
            text: (root.draft.uncertainties || []).concat((root.draft.sources || []).map(source => i18n("Unverified source hint: %1", source))).join("\n")
            visible: text.length > 0
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
        QQC2.CheckBox { id: showContext; text: i18n("Show the exact context sent to the model") }
        QQC2.ScrollView {
            visible: showContext.checked
            Layout.fillWidth: true
            Layout.preferredHeight: 130
            QQC2.TextArea { text: review.aiRequest; readOnly: true; textFormat: TextEdit.PlainText; selectByMouse: true; wrapMode: Text.Wrap }
        }
        QQC2.CheckBox { id: showExisting; visible: root.validity !== "absent"; text: i18n("Show the preserved structure record") }
        QQC2.ScrollView {
            visible: showExisting.checked
            Layout.fillWidth: true
            Layout.preferredHeight: 130
            QQC2.TextArea { text: JSON.stringify(review.snapshot.existing?.record || {}, null, 2); readOnly: true; textFormat: TextEdit.PlainText; selectByMouse: true; wrapMode: Text.Wrap }
        }
        QQC2.BusyIndicator { running: review.busy; visible: running; Layout.preferredHeight: 32 }
    }
    footer: QQC2.DialogButtonBox {
        QQC2.Button {
            text: i18n("Cancel Analysis")
            visible: review.canCancel
            onClicked: review.cancel()
        }
        QQC2.Button {
            text: i18n("Replace review with an AI proposal")
            enabled: !review.busy && !!review.snapshot.snapshotId && root.validity !== "unsupported" && root.validity !== "invalid"
            onClicked: review.askModel()
        }
        QQC2.Button {
            objectName: "workStructureWriteButton"
            text: i18n("Confirm and Write to EPUB")
            enabled: !review.busy && !review.readOnly && root.rows.some(row => row.confirmed) && root.validity !== "unsupported" && root.validity !== "invalid"
            onClicked: review.commit(root.submittedDraft())
        }
        QQC2.Button {
            text: root.dirty ? i18n("Discard Unwritten Changes and Close") : i18n("Close")
            enabled: !review.busy
            onClicked: root.close()
        }
    }
}
