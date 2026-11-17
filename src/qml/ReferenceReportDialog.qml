// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.arianna as Arianna

QQC2.Dialog {
    id: root

    property var report: ({})
    property bool running: false
    property var referenceEditor: null
    property string actionStatus: ""
    property var editorSessions: ({})
    property string lastEditorSessionId: ""
    property bool lastEditorPending: false
    property string workingCopySessionId: ""
    property string contextMessage: ""
    property bool canRetryImport: false
    signal recheckRequested()
    signal removeOrphanRequested(var removal)
    signal retryImportRequested()
    readonly property var results: report.results || []
    readonly property var filteredResults: results.filter(row => {
        if (problemsOnly.checked && row.status === "ok") {
            return false;
        }
        const search = searchField.text.trim().toLowerCase();
        return !search || root.details(row).toLowerCase().includes(search);
    })

    modal: true
    focus: true
    title: i18nc("@title:window", "Cross-Reference Check")
    standardButtons: QQC2.Dialog.Close
    width: parent ? Math.min(Kirigami.Units.gridUnit * 54, parent.width - Kirigami.Units.largeSpacing * 2) : Kirigami.Units.gridUnit * 54
    height: parent ? Math.min(Kirigami.Units.gridUnit * 44, parent.height - Kirigami.Units.largeSpacing * 2) : Kirigami.Units.gridUnit * 44

    function direction(row) {
        switch (row.direction) {
        case "annotation": return i18n("Remark");
        case "incoming": return i18n("Incoming reference");
        case "outgoing": return i18n("Outgoing reference");
        default: return i18n("Cross-reference inspection");
        }
    }

    function status(row) {
        return row.status === "ok" ? i18n("OK") : row.status === "error" ? i18n("Error") : i18n("Not fully verified");
    }

    function details(row) {
        const lines = [root.status(row) + " · " + root.direction(row)];
        if (row.annotationId) lines.push(i18n("Remark %1", row.annotationId));
        if (row.anchorId) lines.push(i18n("Remark anchor: %1", row.anchorId));
        if (row.text) lines.push(row.text);
        if (row.note) lines.push(row.note);
        if (row.sourceAnchorTitle) lines.push(row.sourceAnchorTitle);
        if (row.sourceBookTitle || row.sourceBookId) lines.push(i18n("Source book: %1", row.sourceBookTitle || row.sourceBookId));
        if (row.sourceBookFile) lines.push(i18n("Source file: %1", row.sourceBookFile));
        if (row.sourceFile) lines.push(i18n("Document: %1", row.sourceFile));
        if (row.line > 0) lines.push(i18n("Line: %1", row.line));
        if (row.sourcePage > 0) lines.push(i18n("Source page: %1", row.sourcePage));
        if (row.sourceAnnotation > 0) lines.push(i18n("Source annotation: %1", row.sourceAnnotation));
        if (row.sourceAnchorId) lines.push(i18n("Source anchor: %1", row.sourceAnchorId));
        if (row.targetBookTitle || row.targetBookId) lines.push(i18n("Target book: %1", row.targetBookTitle || row.targetBookId));
        if (row.targetBookFile) lines.push(i18n("Target file: %1", row.targetBookFile));
        if (row.targetPage > 0) lines.push(i18n("Target page: %1", row.targetPage));
        if (row.targetLocation) lines.push(i18n("Stored target: %1", row.targetLocation));
        if (row.resolvedTarget) lines.push(i18n("Resolved target: %1", row.resolvedTarget));
        for (const hint of (row.editionHints || [])) {
            lines.push(i18n("Existing edition: %1 · Line %2 · Anchor %3", hint.existingDocument || "", hint.existingLine || "?", hint.anchorId));
            if (hint.importDocument) lines.push(i18n("Import: %1 · Line %2", hint.importDocument, hint.importLine || "?"));
            if (hint.message) lines.push(hint.message);
        }
        lines.push("");
        return lines.concat(row.messages || []).join("\n");
    }

    function openRepairEditor(repair) {
        return openEpubEditor(repair, "");
    }

    function openSourceEditor(source) {
        return openEpubEditor(source, source.anchorId);
    }

    function openEpubEditor(location, anchorId) {
        root.actionStatus = "";
        root.lastEditorSessionId = "";
        root.lastEditorPending = false;
        const command = String(Arianna.Config.editorCommand || "").trim();
        if (!command) {
            root.actionStatus = i18n("Configure an EPUB editor in Settings to open this book.");
            return false;
        }
        const importSession = root.workingCopySessionId ? Arianna.EditorSessionStore.sessionInfo(root.workingCopySessionId) : null;
        const useImportSession = importSession && !importSession.error && importSession.workingCopyPath === location.fileName;
        const session = useImportSession ? Arianna.EditorSessionStore.resumeEditorSession(root.workingCopySessionId)
            : Arianna.EditorSessionStore.createOrReuseEditorSession(location.bookId, location.fileName);
        if (!session || session.error) {
            root.actionStatus = session && session.message ? session.message : i18n("Unable to create an editor session.");
            return false;
        }
        root.editorSessions[session.sessionId] = true;
        const args = [session.workingCopyPath];
        if (anchorId) args.push("--anchor-id", anchorId);
        const exactDocument = location.kind === "target" && !!location.documentFile;
        const started = exactDocument
            ? Arianna.ExternalProcess.startEditorDocumentSession(session.sessionId, command, session.workingCopyPath, location.documentFile,
                location.anchorId || "", location.verseReference || "")
            : Arianna.ExternalProcess.startEditorSession(session.sessionId, command, args);
        if (!started) {
            if (!session.reused && !useImportSession) Arianna.EditorSessionStore.finishSession(session.sessionId);
            delete root.editorSessions[session.sessionId];
            root.actionStatus = i18n("Unable to start the EPUB editor. Check the editor command or close the other editor session.");
            return false;
        }
        root.lastEditorSessionId = session.sessionId;
        root.lastEditorPending = exactDocument && Arianna.ExternalProcess.isSigilEditor(command);
        root.actionStatus = root.lastEditorPending ? i18n("Opening the target document in Sigil…")
            : anchorId ? i18n("Source file opened in the editor at anchor %1.", anchorId)
            : i18n("Book opened in the editor. Insert the anchor at the intended passage and save the book.");
        return true;
    }

    EditionComparisonDialog {
        id: editionComparison
        parent: root.parent
    }

    ReferenceTargetDialog {
        id: referenceTargetDialog
        parent: root.parent
        checker: root.referenceEditor
        sourceEditorStatus: root.actionStatus
        targetEditorHandler: repair => ({success: root.openRepairEditor(repair), message: root.actionStatus,
            sessionId: root.lastEditorSessionId, pending: root.lastEditorPending})
        onSourceEditRequested: source => root.openSourceEditor(source)
        onSaved: message => root.actionStatus = message
    }

    ReferenceTitleDialog {
        id: referenceTitleDialog
        parent: root.parent
        checker: root.referenceEditor
        workingCopySessionId: root.workingCopySessionId
        onSaved: message => root.actionStatus = message
    }

    ReferenceRemovalDialog {
        id: referenceRemovalDialog
        parent: root.parent
        checker: root.referenceEditor
        workingCopySessionId: root.workingCopySessionId
        onFinished: message => root.actionStatus = message
    }

    QQC2.Dialog {
        id: restoreRemarkDialog
        objectName: "restoreRemarkConfirmation"
        property var repair: ({})
        parent: root.parent
        anchors.centerIn: parent
        width: Math.min(root.width, Kirigami.Units.gridUnit * 32)
        modal: true
        title: i18n("Restore Remark Anchor")
        standardButtons: QQC2.Dialog.Yes | QQC2.Dialog.Cancel
        contentItem: ColumnLayout {
            QQC2.Label {
                Layout.fillWidth: true
                text: i18n("Restore the missing anchor at the remark's saved position in this working copy? The text must still match. A backup will be created; the remark will be preserved.")
                    + "\n\n" + (restoreRemarkDialog.repair.text || "") + "\n\n" + (restoreRemarkDialog.repair.fileName || "")
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
            }
        }
        onAccepted: {
            if (root.referenceEditor && !root.running) {
                const result = root.referenceEditor.restoreAnnotationAnchor(repair);
                root.actionStatus = result.message || "";
            }
        }
    }

    QQC2.Dialog {
        id: removeAnchorDialog
        objectName: "removeOrphanConfirmation"
        property var removal: ({})
        parent: root.parent
        anchors.centerIn: parent
        width: Math.min(root.width, Kirigami.Units.gridUnit * 32)
        modal: true
        title: i18n("Remove Orphaned Anchor")
        standardButtons: QQC2.Dialog.Yes | QQC2.Dialog.Cancel
        contentItem: ColumnLayout {
            QQC2.Label {
                Layout.fillWidth: true
                text: i18n("Remove this orphaned anchor from the checked book? The text will be preserved and a backup will be created.")
                    + "\n\n" + (removeAnchorDialog.removal.fileName || "")
                    + "\n" + i18n("Source anchor: %1", removeAnchorDialog.removal.anchorId || i18n("Without ID"))
                    + (removeAnchorDialog.removal.documentFile ? "\n" + removeAnchorDialog.removal.documentFile : "")
                    + (removeAnchorDialog.removal.format === "pdf" ? "\n" + i18n("Source page: %1", removeAnchorDialog.removal.page + 1) : "")
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
            }
        }
        onAccepted: {
            const session = Arianna.EditorSessionStore.activeSessionForBook(removal.bookId);
            const isImportCopy = session && !session.error && session.sessionId === root.workingCopySessionId
                && session.workingCopyPath === removal.fileName;
            if (session && !session.error && (!isImportCopy || Arianna.ExternalProcess.isEditorSessionRunning(session.sessionId))) {
                root.actionStatus = i18n("Close the editor session for this book before removing an anchor.");
                return;
            }
            root.removeOrphanRequested(removal);
        }
    }

    Connections {
        target: Arianna.ExternalProcess
        function onEditorSessionNavigationFinished(sessionId, status, message) {
            if (sessionId !== root.lastEditorSessionId) return;
            root.lastEditorPending = false;
            root.actionStatus = status === "anchor-prepared"
                ? i18n("Target document opened. Review the AI instruction in Sigil's Find and Replace panel, then prepare a proposal.")
                : status === "document-opened"
                ? i18n("Book opened in the editor. Insert the anchor at the intended passage and save the book.") : message;
        }
        function onEditorSessionProcessStarted(sessionId, processId) {
            if (root.editorSessions[sessionId]) Arianna.EditorSessionStore.noteEditorProcessStarted(sessionId, processId);
        }
        function onEditorSessionFileChanged(sessionId, file) {
            if (root.editorSessions[sessionId]) Arianna.EditorSessionStore.scheduleImport(sessionId);
        }
        function onEditorSessionFinished(sessionId, exitCode, exitStatus) {
            if (!root.editorSessions[sessionId]) return;
            Arianna.EditorSessionStore.finishSession(sessionId);
            if (exitCode === -1) root.actionStatus = i18n("Unable to start the EPUB editor. Check the editor command or close the other editor session.");
        }
    }

    Connections {
        target: Arianna.EditorSessionStore
        function onEditorSessionImportFinished(sessionId, status, message, workingCopyPath, newStateId, issues) {
            if (!root.editorSessions[sessionId]) return;
            if (status === "imported") {
                root.actionStatus = i18n("Editor changes imported. Checking references again…");
                root.recheckRequested();
            } else if (status !== "unchanged") {
                root.actionStatus = i18n("Editor changes could not be imported: %1\nWorking copy: %2", message, workingCopyPath);
                if (sessionId === root.workingCopySessionId) root.recheckRequested();
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        QQC2.BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            running: root.running
            visible: running
        }

        QQC2.Label {
            Layout.fillWidth: true
            text: root.running ? i18n("Checking references…") : (root.report.message || "")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.bold: true
        }

        QQC2.Label {
            Layout.fillWidth: true
            visible: root.contextMessage.length > 0
            text: root.contextMessage
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }

        QQC2.Button {
            objectName: "retryEpubImport"
            visible: root.canRetryImport
            enabled: !root.running
            text: i18n("Retry EPUB Import")
            icon.name: "view-refresh"
            onClicked: root.retryImportRequested()
        }

        QQC2.Label {
            Layout.fillWidth: true
            visible: !root.running && !!root.report.checkedAt
            text: (root.report.fileName || "") + "\n" + (root.report.checkedAt || "")
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
        }

        QQC2.Label {
            Layout.fillWidth: true
            text: i18n("Checks cross-references between EPUB and PDF books in the Arianna library. Links within the same book are excluded. Books requiring a password and unsupported targets are reported as not fully verified.")
            wrapMode: Text.Wrap
        }

        QQC2.Label {
            Layout.fillWidth: true
            visible: !root.running && !!root.report.results
            text: i18n("Outgoing: %1 · Incoming: %2", root.report.outgoing || 0, root.report.incoming || 0)
            wrapMode: Text.Wrap
        }

        QQC2.Label {
            Layout.fillWidth: true
            visible: !!root.report.editionComparison && !root.running
            text: root.report.editionComparison ? root.report.editionComparison.message : ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
        QQC2.Button {
            objectName: "compareImportEditions"
            visible: !!root.report.editionComparison
            enabled: !root.running && !!root.report.editionComparison && root.report.editionComparison.available === true
            text: i18n("Compare Editions…")
            icon.name: "document-preview"
            onClicked: editionComparison.showComparison(root.report.editionComparison)
        }

        QQC2.TextField {
            id: searchField
            objectName: "referenceSearch"
            Layout.fillWidth: true
            placeholderText: i18n("Search books, anchors, targets and messages…")
            enabled: !root.running
        }

        QQC2.Label {
            Layout.fillWidth: true
            visible: root.actionStatus.length > 0
            text: root.actionStatus
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }

        GridLayout {
            id: reportActions
            Layout.fillWidth: true
            columns: root.width >= Kirigami.Units.gridUnit * 38 ? 3 : 1

            QQC2.CheckBox {
                id: problemsOnly
                objectName: "referenceProblemsOnly"
                text: i18n("Only problems and unverified references")
            }

            Item {
                visible: reportActions.columns === 3
                Layout.fillWidth: true
            }

            QQC2.Button {
                objectName: "copyReferenceReport"
                text: i18n("Copy Report")
                icon.name: "edit-copy"
                enabled: !root.running && !!root.report.message
                onClicked: Arianna.Clipboard.savePlainText([root.report.fileName || "", root.report.checkedAt || "", root.report.message || "", ""].concat(root.results.map(row => root.details(row))).join("\n\n"))
            }
        }

        ListView {
            id: resultList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: Kirigami.Units.smallSpacing
            model: root.filteredResults
            QQC2.ScrollBar.vertical: QQC2.ScrollBar {}

            delegate: QQC2.Frame {
                id: resultCard
                required property var modelData
                property bool expanded: modelData.status !== "ok"
                width: resultList.width - Kirigami.Units.largeSpacing
                implicitHeight: cardContents.implicitHeight + topPadding + bottomPadding

                ColumnLayout {
                    id: cardContents
                    width: parent.width

                    QQC2.ItemDelegate {
                        Layout.fillWidth: true
                        icon.name: resultCard.expanded ? "arrow-down" : "arrow-right"
                        onClicked: resultCard.expanded = !resultCard.expanded
                        contentItem: QQC2.Label {
                            text: root.status(resultCard.modelData) + " · " + root.direction(resultCard.modelData) + "\n"
                                + (resultCard.modelData.annotationId || resultCard.modelData.sourceAnchorTitle || resultCard.modelData.sourceAnchorId || resultCard.modelData.sourceFile || "")
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            font.bold: true
                            color: resultCard.modelData.status === "error" ? Kirigami.Theme.negativeTextColor
                                : resultCard.modelData.status === "ok" ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.neutralTextColor
                        }
                    }

                    QQC2.TextArea {
                        visible: resultCard.expanded
                        Layout.fillWidth: true
                        text: root.details(resultCard.modelData)
                        textFormat: TextEdit.PlainText
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        QQC2.Button {
                            objectName: "editCrossReference"
                            Layout.fillWidth: true
                            visible: !!resultCard.modelData.referenceEdit && !!root.referenceEditor
                            enabled: !root.running
                            text: i18n("Edit Cross-Reference")
                            icon.name: "document-edit"
                            onClicked: referenceTargetDialog.edit(resultCard.modelData)
                        }

                        QQC2.Button {
                            objectName: "deleteCrossReference"
                            Layout.fillWidth: true
                            visible: !!resultCard.modelData.referenceEdit && !!root.referenceEditor
                            enabled: !root.running
                            text: i18n("Delete Cross-Reference")
                            icon.name: "edit-delete"
                            onClicked: referenceRemovalDialog.confirmRemoval(resultCard.modelData.referenceEdit)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        QQC2.Button {
                            objectName: "editReferenceSource"
                            Layout.fillWidth: true
                            visible: !!resultCard.modelData.sourceEditor
                            enabled: !root.running
                            text: i18n("Edit Source File")
                            icon.name: "document-edit"
                            onClicked: root.openSourceEditor(resultCard.modelData.sourceEditor)
                        }

                        QQC2.Button {
                            objectName: "editReferenceTitle"
                            Layout.fillWidth: true
                            visible: !!resultCard.modelData.sourceTitleEdit && !!root.referenceEditor
                            enabled: !root.running
                            text: i18n("Edit EPUB Anchor Title")
                            icon.name: "document-edit"
                            onClicked: referenceTitleDialog.editTitle(resultCard.modelData)
                        }
                    }

                    QQC2.Button {
                        objectName: "restoreRemarkAnchor"
                        visible: !!resultCard.modelData.annotationRepair && !!root.referenceEditor
                        enabled: !root.running
                        text: i18n("Restore Remark Anchor…")
                        icon.name: "edit-undo"
                        onClicked: {
                            restoreRemarkDialog.repair = resultCard.modelData.annotationRepair;
                            restoreRemarkDialog.open();
                        }
                    }

                    QQC2.Button {
                        objectName: "removeOrphanAnchor"
                        visible: resultCard.expanded && !!resultCard.modelData.removal
                        enabled: !root.running
                        text: i18n("Remove Orphaned Anchor")
                        icon.name: "edit-delete"
                        onClicked: {
                            removeAnchorDialog.removal = resultCard.modelData.removal;
                            removeAnchorDialog.open();
                        }
                    }

                    Repeater {
                        model: resultCard.expanded ? (resultCard.modelData.editionHints || []) : []
                        delegate: QQC2.Button {
                            required property var modelData
                            objectName: "compareAnchorEdition"
                            text: i18n("Compare Anchor %1…", modelData.anchorId)
                            icon.name: "document-preview"
                            enabled: !root.running
                            onClicked: editionComparison.showComparison(root.report.editionComparison, modelData)
                        }
                    }

                    Repeater {
                        model: resultCard.expanded ? (resultCard.modelData.repairs
                            || (resultCard.modelData.targetAnchorRepair ? [resultCard.modelData.targetAnchorRepair] : [])) : []

                        delegate: ColumnLayout {
                            id: repairActions
                            required property var modelData
                            Layout.fillWidth: true

                            QQC2.Label {
                                Layout.fillWidth: true
                                text: repairActions.modelData.kind === "annotation" ? i18n("Missing remark anchor")
                                    : repairActions.modelData.kind === "source" ? i18n("Missing source anchor") : i18n("Missing target anchor")
                                font.bold: true
                            }

                            QQC2.Label {
                                Layout.fillWidth: true
                                text: repairActions.modelData.fileName + (repairActions.modelData.documentFile ? "\n" + repairActions.modelData.documentFile : "")
                                textFormat: Text.PlainText
                                wrapMode: Text.WrapAnywhere
                            }

                            QQC2.TextArea {
                                Layout.fillWidth: true
                                text: repairActions.modelData.htmlSource
                                textFormat: TextEdit.PlainText
                                readOnly: true
                                selectByMouse: true
                                wrapMode: TextEdit.Wrap
                            }

                            QQC2.Label {
                                Layout.fillWidth: true
                                text: repairActions.modelData.fromExistingEdition
                                    ? i18n("Original HTML from the existing edition. Compare the passage before inserting it into the import.")
                                    : repairActions.modelData.kind === "source"
                                    ? i18n("Template: replace the placeholder with the original reference text and insert it at the intended passage.")
                                    : i18n("Insert this anchor at the intended target passage in the specified EPUB document.")
                                wrapMode: Text.Wrap
                            }

                            RowLayout {
                                QQC2.Button {
                                    objectName: "openMissingAnchorEditor"
                                    text: i18n("Open EPUB in Editor")
                                    icon.name: "document-edit"
                                    enabled: !root.running
                                    onClicked: root.openRepairEditor(repairActions.modelData)
                                }
                                QQC2.Button {
                                    objectName: "copyMissingAnchorHtml"
                                    text: i18n("Copy Anchor HTML")
                                    icon.name: "edit-copy"
                                    onClicked: {
                                        Arianna.Clipboard.savePlainText(repairActions.modelData.htmlSource);
                                        root.actionStatus = i18n("Anchor HTML copied.");
                                    }
                                }
                            }
                        }
                    }
                }
            }

            QQC2.Label {
                anchors.centerIn: parent
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                visible: !root.running && !!root.report.results && resultList.count === 0
                text: root.results.length === 0 ? i18n("No references found.") : i18n("No results match the filter.")
                wrapMode: Text.Wrap
            }
        }
    }
}
