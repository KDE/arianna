// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.arianna as Arianna

QQC2.Dialog {
    id: root

    property var checker: null
    property var edit: ({})
    property string workingCopySessionId: ""
    property string errorMessage: ""
    signal saved(string message)

    objectName: "referenceTitleDialog"
    title: i18n("Edit EPUB Anchor Title")
    modal: true
    focus: true
    standardButtons: QQC2.Dialog.Cancel
    width: parent ? Math.min(Kirigami.Units.gridUnit * 36, parent.width - Kirigami.Units.largeSpacing * 2) : Kirigami.Units.gridUnit * 36
    anchors.centerIn: parent

    function editTitle(finding) {
        edit = finding.sourceTitleEdit;
        anchorTitle.text = edit.htmlTitle || "";
        errorMessage = "";
        open();
        anchorTitle.forceActiveFocus();
        anchorTitle.selectAll();
    }

    function saveTitle() {
        const session = Arianna.EditorSessionStore.activeSessionForBook(edit.bookId);
        const isImportCopy = session && !session.error && session.sessionId === workingCopySessionId && session.workingCopyPath === edit.fileName;
        if (session && !session.error && (!isImportCopy || Arianna.ExternalProcess.isEditorSessionRunning(session.sessionId))) {
            errorMessage = i18n("Close the editor session for this book before changing an anchor title.");
            return;
        }
        const result = checker.saveReferenceTitle(edit, anchorTitle.text);
        if (result.success) {
            saved(result.message);
            close();
        } else {
            errorMessage = result.message;
        }
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        QQC2.Label {
            Layout.fillWidth: true
            text: (root.edit.fileName || "") + "\n" + (root.edit.documentFile || "")
                + "\n" + i18n("Source anchor: %1", root.edit.reference ? root.edit.reference.sourceAnchorId : "")
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
        }

        QQC2.Label {
            text: i18n("EPUB anchor title (title)")
        }

        QQC2.TextField {
            id: anchorTitle
            objectName: "referenceAnchorTitle"
            Layout.fillWidth: true
            placeholderText: i18n("For example: S.B. 3.2.2")
            selectByMouse: true
            onAccepted: if (save.enabled) root.saveTitle()
        }

        QQC2.Label {
            Layout.fillWidth: true
            text: i18n("Saves the title in the displayed EPUB file and the stored cross-reference. An empty title removes the title attribute. A backup is created before changing the EPUB.")
            wrapMode: Text.Wrap
        }

        QQC2.Label {
            objectName: "referenceTitleError"
            Layout.fillWidth: true
            visible: root.errorMessage.length > 0
            text: root.errorMessage
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            color: Kirigami.Theme.negativeTextColor
        }

        QQC2.Button {
            id: save
            objectName: "saveReferenceTitle"
            text: i18n("Save Anchor Title")
            icon.name: "document-save"
            enabled: !!root.checker && !root.checker.running
            onClicked: root.saveTitle()
        }
    }
}
