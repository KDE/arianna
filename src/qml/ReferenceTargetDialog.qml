// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

QQC2.Dialog {
    id: root

    property var checker: null
    property var reference: ({})
    property var row: ({})
    property var targetBooks: []
    property var validation: ({})
    property string sourceEditorStatus: ""
    property bool sourceEditorRequested: false
    property var targetEditorHandler: null
    signal saved(string message)
    signal sourceEditRequested(var source)

    objectName: "referenceTargetDialog"
    title: i18n("Edit Cross-Reference")
    modal: true
    focus: true
    standardButtons: QQC2.Dialog.Cancel
    width: parent ? Math.min(Kirigami.Units.gridUnit * 42, parent.width - Kirigami.Units.largeSpacing * 2) : Kirigami.Units.gridUnit * 42
    anchors.centerIn: parent

    function edit(finding) {
        row = finding;
        reference = finding.referenceEdit;
        targetBooks = checker.referenceTargetBooks(reference);
        targetBook.currentIndex = targetBooks.findIndex(book => book.selected);
        targetLocation.text = reference.targetLocation || "";
        validation = ({});
        sourceEditorRequested = false;
        open();
        targetLocation.forceActiveFocus();
    }

    function checkAndSave(save) {
        if (!checker || targetBook.currentIndex < 0) return;
        const bookId = targetBooks[targetBook.currentIndex].bookId;
        validation = save ? checker.saveReferenceTarget(reference, bookId, targetLocation.text)
            : checker.validateReferenceTarget(reference, bookId, targetLocation.text);
        if (save && validation.success) {
            saved(validation.message);
            close();
        }
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        QQC2.Label {
            Layout.fillWidth: true
            text: i18n("Source book: %1", root.row.sourceBookTitle || root.reference.sourceBookId || "")
                + "\n" + i18n("Source anchor: %1", root.reference.sourceAnchorId || "")
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
        }

        QQC2.Label {
            Layout.fillWidth: true
            visible: root.sourceEditorRequested && root.sourceEditorStatus.length > 0
            text: root.sourceEditorStatus
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
        }

        QQC2.Button {
            objectName: "editReferenceSourceInDialog"
            visible: !!root.row.sourceEditor
            enabled: !!root.checker && !root.checker.running
            text: i18n("Edit Source File")
            icon.name: "document-edit"
            onClicked: {
                root.sourceEditorRequested = true;
                root.sourceEditRequested(root.row.sourceEditor);
            }
        }

        QQC2.Label {
            text: i18n("Target book")
        }

        QQC2.ComboBox {
            id: targetBook
            objectName: "referenceTargetBook"
            Layout.fillWidth: true
            model: root.targetBooks
            textRole: "displayName"
            displayText: currentIndex < 0 ? i18n("Select a target book…") : currentText
            onCurrentIndexChanged: root.validation = ({})
        }

        QQC2.Label {
            Layout.fillWidth: true
            text: targetBook.currentIndex >= 0 ? root.targetBooks[targetBook.currentIndex].fileName : ""
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
        }

        QQC2.Label {
            text: i18n("Target location (targetLocation)")
        }

        QQC2.ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 5
            QQC2.TextArea {
                id: targetLocation
                objectName: "referenceTargetLocation"
                placeholderText: i18n("EPUB: chapter.xhtml#anchor · PDF: stored page location (JSON)")
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.WrapAnywhere
                selectByMouse: true
                onTextChanged: root.validation = ({})
            }
        }

        QQC2.Label {
            Layout.fillWidth: true
            text: i18n("The target must exist and be unambiguous. Saving updates the stored cross-reference immediately.")
            wrapMode: Text.Wrap
        }

        QQC2.Label {
            objectName: "referenceTargetValidation"
            Layout.fillWidth: true
            visible: !!root.validation.message
            text: root.validation.message || ""
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            color: root.validation.success ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor
        }

        Flow {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            QQC2.Button {
                objectName: "validateReferenceTarget"
                text: i18n("Check Target")
                icon.name: "edit-find"
                enabled: !!root.checker && !root.checker.running && targetBook.currentIndex >= 0 && targetLocation.text.trim().length > 0
                onClicked: root.checkAndSave(false)
            }
            QQC2.Button {
                objectName: "saveReferenceTarget"
                text: i18n("Check Target and Save")
                icon.name: "document-save"
                enabled: !!root.checker && !root.checker.running && targetBook.currentIndex >= 0 && targetLocation.text.trim().length > 0
                onClicked: root.checkAndSave(true)
            }
        }
        TargetAnchorRepair {
            Layout.fillWidth: true
            repair: root.validation.targetAnchorRepair || ({})
            enabled: !!root.checker && !root.checker.running
            editorHandler: root.targetEditorHandler
        }
    }
}
