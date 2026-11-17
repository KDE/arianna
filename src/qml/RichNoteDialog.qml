// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.arianna
Kirigami.Dialog {
    id: root
    objectName: "richNoteDialog"
    property var annotation: ({})
    property string bookId: ""
    property string draft: ""
    property string html: ""
    property string errorMessage: ""
    property bool editing: true
    property bool canEdit: true
    property alias highlightColor: annotationColors.selectedColor
    property var discussionTurns: []
    property int discussionRevision: 0
    readonly property bool discussion: annotation.noteMetadata?.origin === "ai-discussion"
        && annotation.noteMetadata?.kind === "conversation"
    readonly property bool validDiscussion: {
        discussionRevision;
        let characters = 0;
        let hasAnswer = false;
        for (const turn of discussionTurns) {
            if (!turn.question.trim().length) return false;
            characters += turn.question.length + turn.answer.length;
            hasAnswer = hasAnswer || turn.answer.trim().length > 0;
        }
        return discussionTurns.length > 0 && characters <= 256 * 1024 && hasAnswer;
    }
    signal saveRequested(string bookId, var annotation, string markdown)
    signal saveDiscussionRequested(string bookId, var annotation, var turns)
    function updateDiscussionTurn(index, field, text) {
        if (!discussionTurns[index] || discussionTurns[index][field] === text) return;
        discussionTurns[index][field] = text;
        discussionRevision++;
        renderTimer.restart();
    }
    function openDraft(source, target, metadata) {
        annotation = Object.assign({}, target, {noteMetadata:metadata});
        bookId = target.bookId;
        draft = source;
        editing = true;
        highlightColor = target.color || "yellow";
        errorMessage = "";
        tabs.currentIndex = 0;
        html = AnnotationStore.renderNote(draft).noteHtml || "";
        open();
    }
    function openSaved(note, id, edit) {
        annotation = JSON.parse(JSON.stringify(note));
        bookId = id;
        draft = note.noteSource || note.note || "";
        discussionTurns = discussion ? JSON.parse(JSON.stringify(note.noteMetadata.turns)) : [];
        discussionRevision++;
        editing = !!edit;
        highlightColor = note.color || "yellow";
        errorMessage = "";
        tabs.currentIndex = editing ? 0 : 1;
        html = discussion ? (AnnotationStore.renderDiscussion(note.noteMetadata.turns).noteHtml || "") : (note.noteHtml || "");
        open();
    }
    onDraftChanged: renderTimer.restart()
    onClosed: { renderTimer.stop(); annotation = ({}); discussionTurns = []; draft = ""; html = ""; }
    title: editing ? i18n("Edit AI Remark") : discussion ? i18n("AI Discussion Remark") : i18n("AI Remark")
    modal: true
    standardButtons: QQC2.Dialog.NoButton
    width: Math.min(Kirigami.Units.gridUnit * 44, parent ? parent.width - 24 : 800)
    height: Math.min(Kirigami.Units.gridUnit * 36, parent ? parent.height - 24 : 720)
    contentItem: ColumnLayout {
        QQC2.Label {
            Layout.fillWidth: true
            text: String(root.annotation.text || "")
            maximumLineCount: 3; elide: Text.ElideRight; wrapMode: Text.Wrap
            textFormat: Text.PlainText
        }
        QQC2.TabBar {
            id: tabs
            objectName: "aiRemarkTabs"
            Layout.fillWidth: true
            visible: root.editing
            QQC2.TabButton { text: i18n("Text (Markdown)") }
            QQC2.TabButton { text: i18n("Preview") }
        }
        AnnotationColorPalette {
            id: annotationColors
            objectName: "richNoteAnnotationColors"
            visible: root.editing
            enabled: root.canEdit
        }
        QQC2.ScrollView {
            id: editScroll
            Layout.fillWidth: true; Layout.fillHeight: true
            visible: root.editing && !root.discussion && tabs.currentIndex === 0
            QQC2.TextArea {
                objectName: "aiRemarkDraft"
                width: editScroll.availableWidth
                text: root.draft
                onTextChanged: if (root.draft !== text) root.draft = text
                textFormat: TextEdit.PlainText
                selectByMouse: true; wrapMode: Text.Wrap
            }
        }
        QQC2.ScrollView {
            id: discussionEditScroll
            Layout.fillWidth: true; Layout.fillHeight: true
            visible: root.editing && root.discussion && tabs.currentIndex === 0
            contentWidth: availableWidth
            ColumnLayout {
                width: discussionEditScroll.availableWidth
                Repeater {
                    model: root.discussionTurns
                    delegate: ColumnLayout {
                        id: turnEditor
                        required property int index
                        required property var modelData
                        Layout.fillWidth: true
                        QQC2.Label { text: i18n("User"); font.bold: true }
                        QQC2.TextArea {
                            objectName: "aiDiscussionRemarkQuestion" + turnEditor.index
                            Layout.fillWidth: true
                            text: turnEditor.modelData.question
                            onTextChanged: root.updateDiscussionTurn(turnEditor.index, "question", text)
                            textFormat: TextEdit.PlainText
                            selectByMouse: true; wrapMode: Text.Wrap
                            Accessible.name: i18n("User")
                        }
                        QQC2.Label { text: i18n("AI"); font.bold: true }
                        QQC2.TextArea {
                            objectName: "aiDiscussionRemarkAnswer" + turnEditor.index
                            Layout.fillWidth: true
                            text: turnEditor.modelData.answer
                            onTextChanged: root.updateDiscussionTurn(turnEditor.index, "answer", text)
                            textFormat: TextEdit.PlainText
                            selectByMouse: true; wrapMode: Text.Wrap
                            Accessible.name: i18n("AI")
                        }
                    }
                }
            }
        }
        Loader {
            Layout.fillWidth: true; Layout.fillHeight: true
            active: root.visible && (!root.editing || tabs.currentIndex === 1)
            visible: active
            sourceComponent: NoteHtmlView { html: root.html }
        }
        QQC2.Label {
            Layout.fillWidth: true
            visible: root.errorMessage.length > 0
            text: root.errorMessage; color: Kirigami.Theme.negativeTextColor
            wrapMode: Text.Wrap; textFormat: Text.PlainText
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            QQC2.Button {
                objectName: "aiRemarkEditButton"
                text: i18n("Edit")
                visible: !root.editing && root.canEdit
                onClicked: { root.editing = true; tabs.currentIndex = 0; }
            }
            QQC2.Button {
                objectName: "aiRemarkSaveButton"
                text: i18n("Save Remark")
                visible: root.editing
                enabled: root.canEdit && (root.discussion ? root.validDiscussion : root.draft.trim().length > 0 && root.draft.length <= 256 * 1024)
                onClicked: {
                    const edited = Object.assign({}, root.annotation, {color: root.highlightColor});
                    if (root.discussion) root.saveDiscussionRequested(root.bookId, edited, root.discussionTurns);
                    else root.saveRequested(root.bookId, edited, root.draft);
                }
            }
            QQC2.Button { text: root.editing ? i18n("Cancel") : i18n("Close"); onClicked: root.close() }
        }
    }
    Timer {
        id: renderTimer; interval: 200
        onTriggered: if (root.visible && root.editing) {
            root.html = (root.discussion ? AnnotationStore.renderDiscussion(root.discussionTurns) : AnnotationStore.renderNote(root.draft)).noteHtml || "";
        }
    }
}
