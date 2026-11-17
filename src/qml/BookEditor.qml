// SPDX-License-Identifier: GPL-2.0-or-later
import QtQuick
import org.kde.arianna

QtObject {
    function openBook(fileName, entry, cfi, selectedText, documentFile, anchorId, verseReference) {
        const isPdf = fileName && fileName.toString().toLowerCase().endsWith(".pdf");
        const pdfCommand = (Config.pdfEditorCommand || "").trim();
        const command = isPdf ? pdfCommand : (Config.editorCommand || "").trim();
        if (!fileName || !command) {
            return { success: false, message: i18n("Configure an EPUB editor in Settings to open this book.") };
        }

        if (isPdf) {
            const started = ExternalProcess.startDetached(pdfCommand, [fileName]);
            if (!started) console.warn("Unable to start PDF editor for:", fileName);
            return { success: started };
        }

        const bookId = entry ? (entry.uniqueIdentifier || entry.identifier || "") : "";
        const session = EditorSessionStore.createOrReuseEditorSession(bookId, fileName);
        if (!session || session.error) {
            console.warn("Unable to create editor session:", session && session.message ? session.message : "");
            return { success: false, message: session && session.message ? session.message : i18n("Unable to create an editor session.") };
        }

        const editorArguments = [session.workingCopyPath];
        if (cfi && cfi.length > 0) editorArguments.push("--goto-cfi", cfi);
        if (selectedText && selectedText.length > 0) editorArguments.push("--select-text", selectedText);
        const exactDocument = !!documentFile;
        const started = exactDocument
            ? ExternalProcess.startEditorDocumentSession(session.sessionId, command, session.workingCopyPath, documentFile, anchorId || "", verseReference || "")
            : ExternalProcess.startEditorSession(session.sessionId, command, editorArguments);
        if (!started) {
            if (!session.reused) {
                EditorSessionStore.finishSession(session.sessionId);
            }
            const activated = !exactDocument && ExternalProcess.activateWindowForFile(session.workingCopyPath);
            if (!activated) console.warn("Unable to start or activate editor for:", session.workingCopyPath);
            return { success: activated, message: activated ? i18n("The book is already open in the editor.")
                : i18n("Unable to start the EPUB editor. Check the editor command or close the other editor session.") };
        }
        return { success: true, sessionId: session.sessionId, pending: exactDocument && ExternalProcess.isSigilEditor(command),
            message: i18n("Opening the EPUB editor at the selected text.") };
    }
}
