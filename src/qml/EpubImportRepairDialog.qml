// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import QtQuick
import org.kde.arianna as Arianna

ReferenceReportDialog {
    id: root

    property string importBookId: ""
    property string importFileName: ""
    property bool openOnlyOnProblems: false
    property var pendingCheck: null
    property var heldSessions: []
    property bool releaseAfterCheck: false

    function releaseCopies() {
        for (const id of heldSessions) Arianna.EditorSessionStore.releaseWorkingCopy(id);
        heldSessions = [];
    }

    onAboutToHide: {
        pendingCheck = null;
        releaseAfterCheck = true;
    }
    onClosed: if (!checker.running) releaseCopies()
    Component.onDestruction: releaseCopies()
    signal retryActiveImportRequested(string bookId)

    title: i18n("Check Remarks and Cross-References on EPUB Import")
    report: checker.report
    running: checker.running
    referenceEditor: checker
    canRetryImport: importBookId.length > 0
    contextMessage: i18n("Compare the imported EPUB with the existing edition to restore missing remark and cross-reference anchors. After correcting the anchors, retry the import.")

    function showForImport(bookId, fileName, sessionId, onlyProblems = false) {
        releaseAfterCheck = false;
        if (sessionId && !heldSessions.includes(sessionId)) {
            Arianna.EditorSessionStore.retainWorkingCopy(sessionId);
            heldSessions = [...heldSessions, sessionId];
        }
        const request = {bookId: bookId, fileName: fileName, sessionId: sessionId || "", onlyProblems: onlyProblems};
        if (checker.running) {
            pendingCheck = request;
            return;
        }
        importBookId = request.bookId;
        importFileName = request.fileName;
        workingCopySessionId = request.sessionId;
        openOnlyOnProblems = request.onlyProblems;
        checker.startWorkingCopy(importBookId, importFileName);
        if (!onlyProblems) open();
    }

    function recheck() {
        showForImport(importBookId, importFileName, workingCopySessionId, !visible);
    }

    onRecheckRequested: recheck()
    onRemoveOrphanRequested: removal => {
        const result = checker.removeOrphanAnchor(removal);
        actionStatus = result.message || "";
    }
    onRetryImportRequested: {
        if (workingCopySessionId) {
            const result = Arianna.EditorSessionStore.importNow(workingCopySessionId);
            if (result.status === "imported" || result.status === "unchanged") {
                actionStatus = i18n("EPUB import completed.");
            } else {
                actionStatus = result.message || i18n("The EPUB could not be imported. Correct the reported anchors and try again.");
            }
            recheck();
        } else {
            retryActiveImportRequested(importBookId);
        }
    }

    Arianna.BookReferenceCheck {
        id: checker
        onReportChanged: {
            if (running || !report.message) return;
            if (root.pendingCheck) {
                const request = root.pendingCheck;
                root.pendingCheck = null;
                Qt.callLater(() => {
                    if (!root.releaseAfterCheck)
                        root.showForImport(request.bookId, request.fileName, request.sessionId, request.onlyProblems);
                });
                return;
            }
            if (root.releaseAfterCheck) root.releaseCopies();
            else if (!root.openOnlyOnProblems || !report.success) root.open();
            else if (!root.visible) root.releaseCopies();
        }
    }
}
