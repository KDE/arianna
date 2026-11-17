// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.arianna

ColumnLayout {
    id: root
    property var repair: ({})
    property bool requested: false
    property string actionStatus: ""
    property var editorHandler: null
    property string editorSessionId: ""
    property string verseReference: ""
    visible: !!repair.fileName && !!repair.bookId && !!repair.anchorId
    spacing: Kirigami.Units.smallSpacing
    onRepairChanged: { requested = false; actionStatus = ""; editorSessionId = ""; verseReference = repair.verseReference || ""; }

    BookEditor { id: editor }
    function openEditor() {
        if (!visible || !enabled || requested) return;
        const context = Object.assign({}, repair, {verseReference: root.verseReference});
        const result = editorHandler ? editorHandler(context)
            : editor.openBook(repair.fileName, {uniqueIdentifier: repair.bookId}, "", "", repair.documentFile, repair.anchorId, root.verseReference);
        requested = !!result.success;
        editorSessionId = result.sessionId || "";
        actionStatus = result.pending ? i18n("Opening the target document in Sigil…") : result.success
            ? i18n("Book opened in the editor. Insert the anchor at the intended passage and save the book.")
            : result.message || i18n("Unable to start the EPUB editor. Check the editor command or close the other editor session.");
    }
    Connections {
        target: ExternalProcess
        function onEditorSessionNavigationFinished(sessionId, status, message) {
            if (sessionId !== root.editorSessionId) return;
            root.requested = status === "anchor-prepared" || status === "document-opened";
            root.actionStatus = status === "anchor-prepared"
                ? i18n("Target document opened. Review the AI instruction in Sigil's Find and Replace panel, then prepare a proposal.")
                : status === "document-opened"
                ? i18n("Book opened in the editor. Insert the anchor at the intended passage and save the book.") : message;
        }
        function onEditorSessionFinished(sessionId, exitCode, exitStatus) {
            if (sessionId !== root.editorSessionId) return;
            root.requested = false;
            if (exitCode === -1) {
                root.actionStatus = i18n("Unable to start the EPUB editor. Check the editor command or close the other editor session.");
            }
        }
    }
    QQC2.Label {
        text: i18n("Missing target anchor")
        font.bold: true
    }
    QQC2.Label {
        Layout.fillWidth: true
        text: (root.repair.documentFile || "") + "#" + (root.repair.anchorId || "")
        textFormat: Text.PlainText
        wrapMode: Text.WrapAnywhere
    }
    QQC2.TextArea {
        objectName: "missingTargetAnchorHtml"
        Layout.fillWidth: true
        text: root.repair.htmlSource || ""
        textFormat: TextEdit.PlainText
        readOnly: true
        selectByMouse: true
        wrapMode: TextEdit.Wrap
    }
    QQC2.Label {
        Layout.fillWidth: true
        text: i18n("Insert this anchor at the intended target passage in the specified EPUB document.")
        wrapMode: Text.Wrap
    }
    QQC2.TextField {
        objectName: "targetAnchorVerseReference"
        Layout.fillWidth: true
        placeholderText: i18n("Verse number or target passage (optional)")
        Accessible.name: placeholderText
        text: root.verseReference
        enabled: !root.requested
        onTextEdited: root.verseReference = text
    }
    Flow {
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing
        QQC2.Button {
            objectName: "openMissingTargetEditor"
            text: i18n("Open Target Book in Editor")
            icon.name: "document-edit"
            enabled: !root.requested
            onClicked: root.openEditor()
        }
        QQC2.Button {
            objectName: "copyMissingTargetAnchor"
            text: i18n("Copy Anchor HTML")
            icon.name: "edit-copy"
            onClicked: { Clipboard.savePlainText(root.repair.htmlSource); root.actionStatus = i18n("Anchor HTML copied."); }
        }
    }
    QQC2.Label {
        objectName: "missingTargetEditorStatus"
        Layout.fillWidth: true
        visible: !!root.actionStatus
        text: root.actionStatus
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
    }
}
