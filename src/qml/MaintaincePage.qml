// SPDX-License-Identifier: GPL-2.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Dialogs as Dialogs
import org.kde.kirigami as Kirigami
import org.kde.arianna

Kirigami.Page {
    id: root
    objectName: "maintenancePage"
    readonly property bool maintenancePage: true
    property bool componentReady: false
    required property LibraryMaintenance checker
    readonly property string checkCategory: checker.category === "references-all" ? "references" : checker.category
    property string exportMessage: ""
    property string textEditorSessionId: ""
    property var selectedPaths: []
    readonly property var rows: checker.report.results || []
    readonly property var orphanedStateRows: rows.filter(row => (row.tableName === "book_state_history" || row.tableName === "book_current_state")
        && (row.codes || []).includes("unassigned-record"))
    readonly property var sortedRows: rows.slice().sort((first, second) => {
        const firstDate = Date.parse(first.createdAt || first.modifiedAt || "") || 0;
        const secondDate = Date.parse(second.createdAt || second.modifiedAt || "") || 0;
        return secondDate - firstDate;
    })
    readonly property var filteredRows: sortedRows.filter(row => statusFilter.currentIndex === 0
        || (statusFilter.currentIndex === 1 && row.status === "error")
        || (statusFilter.currentIndex === 2 && row.status === "warning"))
    readonly property string checkTitle: {
        switch (root.checkCategory) {
        case "database": return i18nc("@action:button", "Database Consistency");
        case "structure": return i18nc("@action:button", "Internal Book Structure");
        case "annotations": return i18nc("@action:button", "Remarks");
        case "references": return i18nc("@action:button", "Cross-references");
        case "cache": return i18nc("@action:button", "Cache and Safety Backups");
        }
        return i18nc("@title:group", "Maintenance");
    }
    title: i18nc("@title:group", "Maintenance")
    readonly property string reportTitle: i18n("Library Maintenance: %1", checkTitle)
    readonly property var checks: [
        { category: "database", label: i18nc("@action:button", "Database Consistency"), icon: "server-database" },
        { category: "structure", label: i18nc("@action:button", "Internal Book Structure"), icon: "document-preview" },
        { category: "annotations", label: i18nc("@action:button", "Remarks"), icon: "document-edit" },
        { category: "references", label: i18nc("@action:button", "Cross-references"), icon: "insert-link" },
        { category: "cache", label: i18nc("@action:button", "Cache and Safety Backups"), icon: "edit-clear-history" }
    ]
    padding: Kirigami.Units.largeSpacing
    implicitWidth: Kirigami.Units.gridUnit * 56
    Component.onCompleted: componentReady = true

    header: QQC2.TabBar {
        id: tabs
        objectName: "maintenanceTabs"
        implicitWidth: Kirigami.Units.gridUnit * 6
        currentIndex: Math.max(0, root.checks.findIndex(check => check.category === root.checkCategory))
        onCurrentIndexChanged: {
            if (root.componentReady && currentIndex >= 0 && currentIndex < root.checks.length
                && root.checks[currentIndex].category !== root.checkCategory) {
                root.showCheck(root.checks[currentIndex].category);
            }
        }
        Repeater {
            model: root.checks
            delegate: QQC2.TabButton {
                required property var modelData
                objectName: "maintenance-" + modelData.category
                text: modelData.label
                icon.name: modelData.icon
                width: implicitWidth
                enabled: !root.checker.running || root.checkCategory === modelData.category
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }
    }

    function showCheck(category) {
        if (checker.running || (category !== "references-all" && !checks.some(check => check.category === category))) {
            return;
        }
        exportMessage = "";
        statusFilter.currentIndex = 0;
        checker.start(category);
        tabs.setCurrentIndex(checks.findIndex(check => check.category === root.checkCategory));
    }

    function selectFile(path, selected) {
        selectedPaths = selected ? [...selectedPaths.filter(value => value !== path), path]
                                 : selectedPaths.filter(value => value !== path);
    }

    function deleteSelected() {
        if (rows.some(row => selectedPaths.includes(row.fileName) && !row.safeToDelete)) {
            deleteConfirmation.paths = selectedPaths.slice();
            deleteConfirmation.open();
        } else {
            checker.cleanCache(selectedPaths, false);
        }
    }

    function deleteSafetyBackup(path) {
        deleteConfirmation.paths = [path];
        deleteConfirmation.open();
    }

    function discardCopy(row) {
        discardConfirmation.fileName = row.fileName;
        discardConfirmation.bookTitle = row.bookTitle || "";
        discardConfirmation.details = (row.messages || []).join("\n");
        discardConfirmation.open();
    }

    function reviewCopy(row) {
        const session = EditorSessionStore.recoverWorkingCopy(row.bookId, row.fileName);
        if (session.error) { exportMessage = session.message; return; }
        recoveryDialog.showForImport(row.bookId, row.fileName, session.sessionId);
    }

    function editReferenceFile(row) {
        applicationWindow().editMaintenanceFile(row.fileName, row.bookId || "");
    }

    function editAnnotationText(row) {
        const command = String(Config.editorCommand || "").trim();
        if (!command) {
            exportMessage = i18n("Configure an EPUB editor in Settings to open this book.");
            return;
        }
        exportMessage = "";
        // Use the remark text as the navigation target; the saved CFI remains
        // available in the report.
        applicationWindow().editMaintenanceFile(row.fileName, row.bookId, "", row.text, result => {
            root.exportMessage = result && result.message ? result.message
                : i18n("Unable to start the EPUB editor. Check the editor command or close the other editor session.");
            root.textEditorSessionId = result && result.sessionId ? result.sessionId : "";
        });
    }

    function anchorRemarkText(row, match) {
        const result = checker.anchorRemarkAtText(row, match);
        exportMessage = result.message || "";
    }

    function findRemarkAnchorText(row) {
        exportMessage = "";
        const result = checker.findRemarkText(row);
        if (!result.success) {
            exportMessage = result.message || i18n("The saved remark text was not found in the EPUB.");
            return;
        }
        if (result.matches.length === 1) {
            anchorRemarkText(row, result.matches[0]);
            return;
        }
        anchorTextChoice.remark = row;
        anchorTextChoice.matches = result.matches;
        anchorTextChoice.open();
    }

    QQC2.Dialog {
        id: anchorTextChoice
        objectName: "anchorTextChoice"
        parent: root.parent
        anchors.centerIn: parent
        modal: true
        title: i18n("Choose Text Passage for Anchor")
        width: root.width * 0.9
        height: Math.min(root.height * 0.8, Kirigami.Units.gridUnit * 28)
        standardButtons: QQC2.Dialog.Cancel
        property var remark: ({})
        property var matches: []
        contentItem: ColumnLayout {
            QQC2.Label {
                Layout.fillWidth: true
                text: i18n("The remark text occurs several times. Choose the correct passage using its EPUB source file and surrounding text (%1 matches shown).", anchorTextChoice.matches.length)
                wrapMode: Text.Wrap
            }
            QQC2.ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                ColumnLayout {
                    width: parent.width
                    Repeater {
                        model: anchorTextChoice.matches
                        delegate: QQC2.ItemDelegate {
                            required property var modelData
                            objectName: "anchorTextMatch"
                            Layout.fillWidth: true
                            text: modelData.documentPath + "\n… " + (modelData.before || "") + " [" + (modelData.text || "") + "] " + (modelData.after || "") + " …"
                            contentItem: ColumnLayout {
                                QQC2.Label {
                                    Layout.fillWidth: true
                                    text: i18n("Occurrence %1 in %2", modelData.occurrence + 1, modelData.documentPath)
                                    font.bold: true
                                    textFormat: Text.PlainText
                                    wrapMode: Text.WrapAnywhere
                                }
                                QQC2.Label {
                                    Layout.fillWidth: true
                                    text: "… " + (modelData.before || "") + " [" + (modelData.text || "") + "] " + (modelData.after || "") + " …"
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                }
                            }
                            onClicked: {
                                const selectedRemark = anchorTextChoice.remark;
                                const selectedMatch = modelData;
                                anchorTextChoice.close();
                                root.anchorRemarkText(selectedRemark, selectedMatch);
                            }
                        }
                    }
                }
            }
        }
    }

    Connections {
        target: ExternalProcess
        function onEditorSessionFinished(sessionId, exitCode, exitStatus) {
            if (sessionId !== root.textEditorSessionId) return;
            root.textEditorSessionId = "";
            if (exitCode === -1)
                root.exportMessage = i18n("Unable to start the EPUB editor. Check the editor command or close the other editor session.");
            else if (exitCode !== 0 || exitStatus !== 0)
                root.exportMessage = i18n("The EPUB editor closed with error code %1.", exitCode);
        }
    }

    Connections {
        target: EditorSessionStore
        function onEditorSessionImportFinished(sessionId, status, message, workingCopyPath, newStateId, anchorValidationIssues) {
            if (!root.visible || (status !== "imported" && status !== "anchor-validation-failed")) {
                return;
            }
            const session = EditorSessionStore.sessionInfo(sessionId);
            if (!session || session.error) {
                return;
            }
            recoveryDialog.actionStatus = status === "anchor-validation-failed" ? message : "";
            recoveryDialog.showForImport(session.bookId, workingCopyPath, sessionId, status === "imported");
        }
    }

    Connections {
        target: root.checker
        function onChanged() {
            if (!root.checker.running) {
                root.selectedPaths = root.rows.filter(row => row.safeToDelete).map(row => row.fileName);
                if (root.checker.report.cleanupMessage) root.exportMessage = root.checker.report.cleanupMessage;
            }
        }
    }

    EpubImportRepairDialog {
        id: recoveryDialog
        parent: root.parent
        onClosed: if (!root.checker.running) root.checker.start("cache")
    }

    QQC2.Dialog {
        id: deleteConfirmation
        objectName: "cacheDeleteConfirmation"
        parent: root.parent
        anchors.centerIn: parent
        modal: true
        title: i18n("Delete Selected Copies?")
        standardButtons: QQC2.Dialog.Yes | QQC2.Dialog.Cancel
        width: root.width * 0.9
        height: Math.min(root.height, Kirigami.Units.gridUnit * 28)
        property var paths: []
        contentItem: ColumnLayout {
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: i18n("The selection includes copies whose successful import has not been verified or the most recent safety backup. Their changes may be lost permanently. Delete the listed files?")
            }
            QQC2.ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                QQC2.TextArea {
                    readOnly: true
                    text: deleteConfirmation.paths.join("\n\n")
                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.Wrap
                }
            }
        }
        onAccepted: root.checker.cleanCache(paths, true)
    }

    QQC2.Dialog {
        id: discardConfirmation
        objectName: "discardWorkingCopyConfirmation"
        parent: root.parent
        anchors.centerIn: parent
        modal: true
        title: i18n("Discard Editor Working Copy?")
        standardButtons: QQC2.Dialog.Yes | QQC2.Dialog.Cancel
        width: root.width * 0.9
        property string fileName: ""
        property string bookTitle: ""
        property string details: ""
        contentItem: QQC2.Label {
            text: i18n("Permanently delete this editor working copy? Unimported changes will be lost. The library book and its saved anchors will not be changed. Afterwards, you can start a new editor session from the current library version.")
                + "\n\n" + discardConfirmation.bookTitle + "\n" + discardConfirmation.fileName
                + "\n\n" + discardConfirmation.details
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
        }
        onAccepted: root.checker.discardWorkingCopy(fileName)
    }

    function combineFormats(plan) {
        // Starting the follow-up check replaces the result delegates immediately.
        // Keep the completion message in the page's surviving context.
        const result = checker.combineFormats(plan);
        exportMessage = result.message;
    }

    function deleteBookFile(path, sharedBookId, deleteFile) {
        const result = sharedBookId.length > 0 ? checker.deleteSharedBookIdFile(path, sharedBookId, deleteFile) : checker.deleteDuplicate(path);
        exportMessage = result.message;
    }

    function requestNewBookId(row, fileName) {
        newBookIdConfirmation.fileName = fileName;
        newBookIdConfirmation.expectedIdentifier = row.bookId || "";
        newBookIdConfirmation.open();
    }

    function assignNewBookId(fileName, expectedIdentifier) {
        const result = checker.assignNewBookId(fileName, expectedIdentifier);
        exportMessage = result.message || "";
        if (result.success) checker.start("database");
    }

    function requestDeleteOrphanedState(row) {
        orphanedHistoryConfirmation.deleteAll = false;
        orphanedHistoryConfirmation.entryCount = 1;
        orphanedHistoryConfirmation.tableName = row.tableName;
        orphanedHistoryConfirmation.bookId = row.bookId;
        orphanedHistoryConfirmation.stateId = row.stateId;
        orphanedHistoryConfirmation.entries = [];
        orphanedHistoryConfirmation.open();
    }

    function deleteOrphanedState(tableName, bookId, stateId) {
        const result = checker.deleteOrphanedBookStateEntry(tableName, bookId, stateId);
        exportMessage = result.message || "";
    }

    function requestDeleteAllOrphanedStates() {
        const entries = orphanedStateRows.map(row => ({ tableName: row.tableName, bookId: row.bookId, stateId: row.stateId }));
        if (entries.length === 0) return;
        orphanedHistoryConfirmation.deleteAll = true;
        orphanedHistoryConfirmation.entryCount = entries.length;
        orphanedHistoryConfirmation.entries = entries;
        orphanedHistoryConfirmation.open();
    }

    function deleteAllOrphanedStates(entries) {
        const result = checker.deleteAllOrphanedBookStates();
        exportMessage = result.message || "";
    }

    QQC2.Dialog {
        id: newBookIdConfirmation
        objectName: "newBookIdConfirmation"
        parent: root.parent
        anchors.centerIn: parent
        modal: true
        title: i18n("Assign a New UUIDv7?")
        standardButtons: QQC2.Dialog.Yes | QQC2.Dialog.Cancel
        width: root.width * 0.9
        property string fileName: ""
        property string expectedIdentifier: ""
        contentItem: QQC2.Label {
            text: newBookIdConfirmation.expectedIdentifier.length > 0
                ? i18n("Assign a new ID to this book file? Existing remarks, cross-references, and saved reading states will not be moved. They still use the old ID and will resolve to the remaining matching book file or files. Review them after this change.\n\nOld ID: %1\nFile: %2",
                       newBookIdConfirmation.expectedIdentifier, newBookIdConfirmation.fileName)
                : i18n("Assign a new ID to this book file?\n\nFile: %1", newBookIdConfirmation.fileName)
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
        }
        onAccepted: root.assignNewBookId(fileName, expectedIdentifier)
    }

    QQC2.Dialog {
        id: orphanedHistoryConfirmation
        objectName: "orphanedHistoryConfirmation"
        parent: root.parent
        anchors.centerIn: parent
        modal: true
        title: i18n("Delete Orphaned State History?")
        standardButtons: QQC2.Dialog.Yes | QQC2.Dialog.Cancel
        width: root.width * 0.9
        property string bookId: ""
        property string stateId: ""
        property string tableName: ""
        property bool deleteAll: false
        property int entryCount: 1
        property var entries: []
        contentItem: QQC2.Label {
            text: orphanedHistoryConfirmation.deleteAll
                  ? i18n("Permanently delete all %1 orphaned saved-state entries from the current report? Book files will not be changed.",
                       orphanedHistoryConfirmation.entryCount)
                  : i18n("Permanently delete this orphaned saved-state entry?\n\nTable: %1\nBook ID: %2\nState ID: %3",
                      orphanedHistoryConfirmation.tableName, orphanedHistoryConfirmation.bookId, orphanedHistoryConfirmation.stateId)
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
        }
        onAccepted: deleteAll ? root.deleteAllOrphanedStates(entries) : root.deleteOrphanedState(tableName, bookId, stateId)
    }

    function escapeHtml(text) {
        return String(text).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&#39;");
    }

    QQC2.Dialog {
        id: duplicateDeleteConfirmation
        objectName: "duplicateDeleteConfirmation"
        parent: root.parent
        anchors.centerIn: parent
        modal: true
        title: sharedBookId.length > 0 ? i18nc("@title:window", "Remove Book") : i18n("Delete Book File?")
        standardButtons: (sharedBookId.length > 0 ? QQC2.Dialog.Ok : QQC2.Dialog.Yes) | QQC2.Dialog.Cancel
        width: root.width * 0.9
        property string fileName: ""
        property string sharedBookId: ""
        function openForBook(path, bookId) {
            fileName = path;
            sharedBookId = bookId;
            deleteBookFileCheckBox.checked = false;
            open();
        }
        contentItem: ColumnLayout {
            spacing: Kirigami.Units.largeSpacing
            QQC2.Label {
                objectName: "bookFileDeleteExplanation"
                Layout.fillWidth: true
                text: duplicateDeleteConfirmation.sharedBookId.length > 0
                    ? i18n("Remove this book from the library? Another file with book ID %1 will remain in the library. Files with the same book ID may have different contents.", duplicateDeleteConfirmation.sharedBookId)
                    : i18n("Permanently delete this file and remove its library entry? Another identical copy will be retained.")
                        + "\n\n" + duplicateDeleteConfirmation.fileName
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing
                visible: duplicateDeleteConfirmation.sharedBookId.length > 0
                QQC2.CheckBox {
                    id: deleteBookFileCheckBox
                    objectName: "deleteSharedBookIdFileCheckBox"
                    Layout.alignment: Qt.AlignTop
                    checked: false
                }
                QQC2.Label {
                    objectName: "deleteSharedBookIdFileLocation"
                    Layout.fillWidth: true
                    text: i18nc("@option:check %1 is the book file location", "Also delete the book from the location:<br><b>%1</b><br>Delete it.", root.escapeHtml(duplicateDeleteConfirmation.fileName))
                    textFormat: Text.RichText
                    wrapMode: Text.WrapAnywhere
                    TapHandler {
                        onTapped: deleteBookFileCheckBox.checked = !deleteBookFileCheckBox.checked
                    }
                }
            }
        }
        onAccepted: root.deleteBookFile(fileName, sharedBookId, deleteBookFileCheckBox.checked)
    }

    function rowText(row, includeFiles = true) {
        const lines = [];
        const reference = !!(row.sourceBookId || row.targetBookId);
        const backup = row.cleanupPlan && row.cleanupPlan.kind === "backup";
        if (!reference) {
            if (backup && row.originalTitle) lines.push(i18n("Original title: %1", row.originalTitle));
            else if (row.bookTitle) lines.push(row.bookTitle);
            if (includeFiles && row.fileName) lines.push(row.fileName);
        }
        if (row.tableName) lines.push(i18n("Table: %1", row.tableName));
        if (row.recordId) lines.push(i18n("Entry: %1 = %2", row.recordKeyField || i18n("ID"), row.recordId));
        if (row.fieldName) lines.push(i18n("ID field: %1", row.fieldName));
        if (row.matchedBookFiles && !row.sharedIdFiles) {
            lines.push(i18n("Matching book files:"));
            lines.push(...row.matchedBookFiles);
        }
        if (row.bytes !== undefined) lines.push(i18n("%1 MiB · Created: %2 · Modified: %3", (row.bytes / 1048576).toFixed(2), row.createdAt || row.modifiedAt, row.modifiedAt));
        if (row.bookId && !reference) lines.push(i18n("Book ID: %1", row.bookId));
        if (row.annotationId) lines.push(i18n("Remark %1", row.annotationId));
        if (row.sourceBookId) lines.push(i18n("Source book: %1 (ID: %2)", row.sourceBookTitle || row.sourceBookId, row.sourceBookId));
        if (row.sourceBookFile && (reference || row.sourceBookFile !== row.fileName)) lines.push(i18n("Source file: %1", row.sourceBookFile));
        if (row.sourceAnchorId) {
            const sourceDocument = row.sourceFile || (row.sourceEditor && row.sourceEditor.documentFile);
            lines.push(sourceDocument
                ? i18n("Source anchor: %1 in %2", row.sourceAnchorId, sourceDocument)
                : i18n("Source anchor: %1", row.sourceAnchorId));
        }
        if (row.targetBookId) lines.push(i18n("Target book: %1 (ID: %2)", row.targetBookTitle || row.targetBookId, row.targetBookId));
        if (row.targetBookFile && row.targetBookFile !== row.fileName) lines.push(i18n("Target file: %1", row.targetBookFile));
        if (row.targetLocation) lines.push(i18n("Stored target: %1", row.targetLocation));
        if (row.text) lines.push(row.text);
        if (row.textMatchCount !== undefined) lines.push(row.textMatchCount === 0 ? i18n("Remark text not found in EPUB.") : i18n("Remark text found in EPUB: %1 matches.", row.textMatchCount));
        if (row.note) lines.push(row.note);
        lines.push(...(row.messages || []));
        if (row.details) lines.push(row.details);
        return lines.join("\n");
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing
        QQC2.Label {
            objectName: "referenceCheckScope"
            Layout.fillWidth: true
            visible: root.checkCategory === "references"
            text: root.checker.category === "references-all"
                ? i18n("Checks the entire library, including cross-reference anchors without a database entry.")
                : i18n("Checks only stored cross-references and their source and target anchors in the linked books.")
            wrapMode: Text.Wrap
        }
        QQC2.Label {
            Layout.fillWidth: true
            text: root.checker.category === "cache"
                ? i18n("Known imported copies are preselected. To abandon a failed import, use Discard Working Copy after closing the editor and import report. Unimported changes will be lost; the library book will be retained. Files in use are protected.")
                : i18n("The check examines the entire library without making changes. Matching EPUB and PDF formats can then be combined into one library entry.")
            wrapMode: Text.Wrap
        }
        QQC2.ProgressBar {
            Layout.fillWidth: true
            visible: root.checker.running
            from: 0
            to: Math.max(1, root.checker.total)
            value: root.checker.completed
            indeterminate: root.checker.total === 0
        }
        QQC2.Label {
            Layout.fillWidth: true
            visible: root.checker.running
            text: i18n("%1 of %2 — %3", root.checker.completed, root.checker.total, root.checker.currentFile)
            elide: Text.ElideMiddle
            textFormat: Text.PlainText
        }
        QQC2.Label {
            objectName: "maintenanceSummary"
            Layout.fillWidth: true
            visible: !root.checker.running && root.checker.report.checkedAt !== undefined
            text: (root.checker.report.cancelled ? i18n("Check cancelled. Results are incomplete.") + "\n" : "")
                + i18n("%1 books in %2 files, %3 remarks, %4 cross-references in the library.",
                       root.checker.report.books || 0, root.checker.report.files || 0,
                       root.checker.report.annotations || 0, root.checker.report.references || 0)
                + "\n" + i18n("%1 OK, %2 errors, %3 notices or incomplete checks.",
                               root.checker.report.passed || 0, root.checker.report.errors || 0, root.checker.report.warnings || 0)
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            QQC2.ComboBox {
                id: statusFilter
                objectName: "maintenanceStatusFilter"
                model: [i18n("All Results"), i18n("Errors"), i18n("Notices / Incomplete")]
                enabled: !root.checker.running
                Layout.fillWidth: true
            }
            QQC2.Button {
                objectName: "cancelMaintenance"
                text: i18n("Cancel Check")
                visible: root.checker.running
                onClicked: root.checker.cancel()
            }
            QQC2.Button {
                objectName: "repeatMaintenance"
                text: i18n("Check Again")
                visible: root.checkCategory !== "references"
                enabled: !root.checker.running
                onClicked: root.showCheck(root.checker.category)
            }
            QQC2.Button {
                objectName: "checkStoredReferences"
                text: i18n("Check Stored Cross-references")
                visible: root.checkCategory === "references"
                enabled: !root.checker.running
                onClicked: root.showCheck("references")
            }
            QQC2.Button {
                objectName: "checkAllLibraryReferences"
                text: i18n("Check Entire Library")
                visible: root.checkCategory === "references"
                enabled: !root.checker.running
                onClicked: root.showCheck("references-all")
            }
        }
        QQC2.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            ListView {
                id: results
                objectName: "maintenanceResults"
                clip: true
                model: root.filteredRows
                spacing: Kirigami.Units.smallSpacing
                delegate: QQC2.ItemDelegate {
                    id: findingDelegate
                    required property var modelData
                    readonly property string sharedBookId: modelData.sharedIdFiles ? modelData.bookId : ""
                    readonly property bool canEditRemarkText: root.checker.category === "annotations"
                        && !!modelData.bookId && !!modelData.fileName && modelData.fileName.toLowerCase().endsWith(".epub")
                        && !!modelData.text && String(modelData.text).trim().length > 0
                        && (modelData.codes || []).some(code => code === "annotation-anchor-unresolved"
                            || code === "annotation-position-missing" || code === "annotation-cfi-unverified")
                    width: ListView.view.width
                    contentItem: ColumnLayout {
                        Image {
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 4
                            Layout.preferredHeight: Kirigami.Units.gridUnit * 6
                            visible: !!modelData.cleanupPlan && modelData.cleanupPlan.kind === "backup" && !!modelData.thumbnail
                            source: visible ? modelData.thumbnail : ""
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            sourceSize.width: width
                            sourceSize.height: height
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: modelData.status === "error" ? i18n("Error")
                                : modelData.status === "warning" ? i18n("Notice / Not fully verified") : i18n("OK")
                            color: modelData.status === "error" ? Kirigami.Theme.negativeTextColor
                                : modelData.status === "warning" ? Kirigami.Theme.neutralTextColor : Kirigami.Theme.positiveTextColor
                            font.bold: true
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: root.rowText(modelData, modelData.duplicateFiles === undefined && modelData.sharedIdFiles === undefined)
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                        }
                        Repeater {
                            model: modelData.sharedIdFiles || modelData.duplicateFiles || []
                            delegate: RowLayout {
                                id: duplicateFile
                                required property string modelData
                                Layout.fillWidth: true
                                QQC2.Label {
                                    Layout.fillWidth: true
                                    text: duplicateFile.modelData
                                    textFormat: Text.PlainText
                                    wrapMode: Text.WrapAnywhere
                                }
                                QQC2.Button {
                                    objectName: "assignNewBookId"
                                    visible: findingDelegate.sharedBookId.length > 0
                                    text: i18n("Assign UUIDv7")
                                    icon.name: "edit-rename"
                                    enabled: !root.checker.running
                                    onClicked: root.requestNewBookId(findingDelegate.modelData, duplicateFile.modelData)
                                }
                                QQC2.Button {
                                    objectName: findingDelegate.sharedBookId.length > 0 ? "deleteSharedBookIdFile" : "deleteDuplicateFile"
                                    readonly property string fileName: duplicateFile.modelData
                                    text: findingDelegate.sharedBookId.length > 0 ? i18nc("@action:button", "Remove Book") : i18nc("@action:button", "Delete File")
                                    icon.name: "edit-delete"
                                    enabled: !root.checker.running && !root.checker.report.cancelled
                                    onClicked: {
                                        duplicateDeleteConfirmation.openForBook(fileName, findingDelegate.sharedBookId);
                                    }
                                }
                            }
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing
                            visible: root.checker.category === "cache" || root.checkCategory === "references"
                                || root.checker.category === "database" || findingDelegate.canEditRemarkText
                            QQC2.CheckBox {
                                objectName: "selectCacheFile"
                                visible: root.checker.category === "cache"
                                text: modelData.safeToDelete ? i18n("Safe to delete") : i18n("Select after review")
                                enabled: modelData.canDelete === true && !root.checker.running
                                checked: root.selectedPaths.includes(modelData.fileName)
                                onToggled: root.selectFile(modelData.fileName, checked)
                            }
                            QQC2.Button {
                                objectName: "assignMissingBookId"
                                visible: root.checker.category === "database" && (modelData.codes || []).includes("missing-book-id")
                                text: i18n("Assign UUIDv7")
                                icon.name: "edit-rename"
                                enabled: !root.checker.running
                                onClicked: root.requestNewBookId(modelData, modelData.fileName)
                            }
                            QQC2.Button {
                                objectName: "deleteOrphanedBookStateHistory"
                                visible: root.checker.category === "database"
                                    && (modelData.tableName === "book_state_history" || modelData.tableName === "book_current_state")
                                    && (modelData.codes || []).includes("unassigned-record")
                                text: i18n("Delete State Entry")
                                icon.name: "edit-delete"
                                enabled: !root.checker.running && !root.checker.report.cancelled
                                onClicked: root.requestDeleteOrphanedState(modelData)
                            }
                            QQC2.Button {
                                objectName: "reviewCacheCopy"
                                text: i18n("Review / Import Copy…")
                                visible: root.checker.category === "cache" && modelData.cleanupPlan && modelData.cleanupPlan.kind === "session" && !!modelData.bookId
                                enabled: modelData.canDelete === true && !root.checker.running
                                onClicked: root.reviewCopy(modelData)
                            }
                            QQC2.Button {
                                objectName: "discardWorkingCopy"
                                visible: root.checker.category === "cache" && modelData.cleanupPlan && modelData.cleanupPlan.kind === "session"
                                enabled: modelData.canDiscard === true && !root.checker.running
                                text: i18n("Discard Working Copy…")
                                icon.name: "edit-delete"
                                onClicked: root.discardCopy(modelData)
                            }
                            QQC2.Button {
                                objectName: "deleteSafetyBackup"
                                visible: root.checker.category === "cache" && modelData.cleanupPlan && modelData.cleanupPlan.kind === "backup"
                                enabled: modelData.canDelete === true && !root.checker.running
                                text: i18n("Delete Safety Backup")
                                icon.name: "edit-delete"
                                onClicked: root.deleteSafetyBackup(modelData.fileName)
                            }
                            QQC2.Button {
                                objectName: "editReferenceFile"
                                visible: root.checkCategory === "references" && modelData.status === "error" && !!modelData.fileName
                                enabled: !root.checker.running
                                text: i18n("Edit File")
                                icon.name: "document-edit"
                                onClicked: root.editReferenceFile(modelData)
                            }
                            QQC2.Button {
                                objectName: "anchorRemarkTextLocation"
                                visible: findingDelegate.canEditRemarkText && modelData.textMatchCount > 0
                                enabled: !root.checker.running && !root.checker.report.cancelled
                                text: i18n("Set Anchor at Text Passage")
                                icon.name: "insert-link"
                                onClicked: root.findRemarkAnchorText(modelData)
                            }
                            QQC2.Button {
                                objectName: "editRemarkTextLocation"
                                visible: findingDelegate.canEditRemarkText
                                enabled: !root.checker.running && !root.checker.report.cancelled
                                text: i18n("Edit Text Passage")
                                icon.name: "document-edit"
                                onClicked: root.editAnnotationText(modelData)
                            }
                        }
                        QQC2.Button {
                            objectName: "combineBookFormats"
                            visible: modelData.formatGroupPlan !== undefined
                            enabled: !root.checker.running
                            text: i18nc("@action:button", "Combine Formats")
                            icon.name: "object-group"
                            onClicked: root.combineFormats(modelData.formatGroupPlan)
                        }
                        TargetAnchorRepair {
                            Layout.fillWidth: true
                            visible: root.checkCategory === "references" && !!repair.fileName
                            repair: findingDelegate.modelData.targetAnchorRepair || ({})
                            enabled: !root.checker.running && !root.checker.report.cancelled
                        }
                    }
                    onClicked: Clipboard.savePlainText(root.rowText(modelData))
                    QQC2.ToolTip.text: i18n("Click to copy this finding.")
                    QQC2.ToolTip.visible: hovered
                }
                QQC2.Label {
                    anchors.centerIn: parent
                    visible: !root.checker.running && results.count === 0
                    text: i18n("No findings for this filter.")
                }
            }
        }
        Flow {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            QQC2.Button {
                objectName: "deleteCacheFiles"
                visible: root.checker.category === "cache"
                text: i18n("Delete Selected (%1)", root.selectedPaths.length)
                enabled: !root.checker.running && !root.checker.report.cancelled && root.selectedPaths.length > 0
                icon.name: "edit-delete"
                onClicked: root.deleteSelected()
            }
            QQC2.Button {
                objectName: "deleteAllOrphanedBookStateHistory"
                visible: root.checker.category === "database" && root.orphanedStateRows.length > 0
                text: i18n("Delete All Orphaned State Entries (%1)", root.orphanedStateRows.length)
                enabled: !root.checker.running && !root.checker.report.cancelled
                icon.name: "edit-delete"
                onClicked: root.requestDeleteAllOrphanedStates()
            }
            QQC2.Button {
                objectName: "copyMaintenanceReport"
                text: i18n("Copy Report")
                enabled: !root.checker.running && root.checker.report.checkedAt !== undefined
                onClicked: Clipboard.savePlainText(root.reportTitle + "\n\n" + root.rows.map(row => root.rowText(row)).join("\n\n"))
            }
            QQC2.Button {
                objectName: "saveMaintenanceReport"
                text: i18n("Save Report…")
                enabled: !root.checker.running && root.checker.report.checkedAt !== undefined
                onClicked: saveDialog.open()
            }
        }
        QQC2.Label {
            Layout.fillWidth: true
            visible: root.exportMessage.length > 0
            text: root.exportMessage
            wrapMode: Text.Wrap
        }
    }
    Dialogs.FileDialog {
        id: saveDialog
        title: i18n("Save Maintenance Report")
        fileMode: Dialogs.FileDialog.SaveFile
        nameFilters: [i18n("JSON reports (*.json)")]
        defaultSuffix: "json"
        onAccepted: root.exportMessage = root.checker.saveReport(selectedFile)
            ? i18n("Report saved.") : i18n("Unable to save the report.")
    }
}
