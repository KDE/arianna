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
    property var plan: ({})
    property string workingCopySessionId: ""
    property string errorMessage: ""
    signal finished(string message)

    objectName: "referenceRemovalConfirmation"
    title: i18n("Delete Cross-Reference")
    modal: true
    focus: true
    standardButtons: QQC2.Dialog.Cancel
    width: parent ? Math.min(Kirigami.Units.gridUnit * 42, parent.width - Kirigami.Units.largeSpacing * 2) : Kirigami.Units.gridUnit * 42
    height: Math.min(implicitHeight, parent ? parent.height - Kirigami.Units.largeSpacing * 2 : implicitHeight)
    anchors.centerIn: parent

    function confirmRemoval(reference) {
        const result = checker.prepareReferenceRemoval(reference);
        if (!result.success) {
            finished(result.message);
            return;
        }
        plan = result.plan;
        errorMessage = "";
        open();
    }

    function removeReference() {
        for (const side of [plan.source, plan.target]) {
            if (side.action === "keep") continue;
            const session = Arianna.EditorSessionStore.activeSessionForBook(side.bookId);
            const isImportCopy = session && !session.error && session.sessionId === workingCopySessionId && session.workingCopyPath === side.fileName;
            if (session && !session.error && (!isImportCopy || Arianna.ExternalProcess.isEditorSessionRunning(session.sessionId))) {
                errorMessage = i18n("Close the affected editor sessions before deleting this cross-reference.");
                return;
            }
        }
        const result = checker.removeReference(plan);
        if (result.success) {
            finished(result.message);
            close();
        } else {
            errorMessage = result.message;
        }
    }

    contentItem: QQC2.ScrollView {
        implicitHeight: contents.implicitHeight
        contentWidth: availableWidth
        ColumnLayout {
            id: contents
            width: parent.width
            spacing: Kirigami.Units.smallSpacing
            QQC2.Label {
                Layout.fillWidth: true
                text: i18n("Delete this cross-reference and apply the following changes to its source and target? The database entry will be deleted. Backups will be created before changing any files.")
                wrapMode: Text.Wrap
            }
            QQC2.Label {
                Layout.fillWidth: true
                text: root.plan.reference ? (root.plan.reference.sourceAnchorTitle || root.plan.reference.sourceAnchorId) : ""
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                font.bold: true
            }
            Repeater {
                model: root.plan.source ? [root.plan.source, root.plan.target] : []
                delegate: QQC2.Label {
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    text: (index === 0 ? i18n("Source book: %1", modelData.bookTitle || "") : i18n("Target book: %1", modelData.bookTitle || ""))
                        + (modelData.fileName ? "\n" + modelData.fileName : "")
                        + (modelData.documentFile ? "\n" + modelData.documentFile : "")
                        + (modelData.anchorId ? "\n" + modelData.anchorId : "")
                        + "\n" + modelData.reason + "\n"
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                }
            }
            QQC2.Label {
                Layout.fillWidth: true
                visible: root.errorMessage.length > 0
                text: root.errorMessage
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                color: Kirigami.Theme.negativeTextColor
            }
            QQC2.Button {
                objectName: "confirmReferenceRemoval"
                text: i18n("Delete Cross-Reference")
                icon.name: "edit-delete"
                enabled: !!root.checker && !root.checker.running
                onClicked: root.removeReference()
            }
        }
    }
}
