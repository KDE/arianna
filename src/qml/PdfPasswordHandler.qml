// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

import QtQuick
import org.kde.arianna

QtObject {
    id: root

    required property OkularPdfDocument document
    required property url source
    property string bookId: ""
    property var window: null
    readonly property bool busy: wallet.busy
    readonly property string identity: source.toString() + "\n" + bookId
    property bool attemptedWallet: false
    property bool submitting: false
    property bool readingWallet: false
    property string sessionPassword: ""
    property string message: ""

    signal passwordRequested(string message)
    signal dismissRequested()
    signal notification(string message)

    function cancel() {
        requestTimer.stop();
        wallet.cancel();
        readingWallet = false;
    }

    function enterManually() {
        cancel();
        attemptedWallet = true;
        if (document && document.needsPassword) {
            passwordRequested(message);
        }
    }

    function submit(password, remember) {
        if (!document || !document.needsPassword) {
            return;
        }
        cancel();
        attemptedWallet = true;
        submitting = true;
        document.password = password;
        submitting = false;
        if (document.status === OkularPdfDocument.Ready) {
            sessionPassword = password;
            message = "";
            if (remember) {
                wallet.save(password);
            }
        } else if (document.needsPassword) {
            sessionPassword = "";
            message = i18nc("@info", "Incorrect PDF password. Please try again.");
            // Reopen after the synchronous passwordRequired/accept sequence.
            requestTimer.restart();
        }
    }

    function requestPassword() {
        if (!document || !document.needsPassword || source.toString().length === 0) {
            return;
        }
        if (sessionPassword.length > 0) {
            submit(sessionPassword, false);
        } else if (!attemptedWallet) {
            attemptedWallet = true;
            readingWallet = true;
            wallet.load();
        } else {
            passwordRequested(message);
        }
    }

    onIdentityChanged: {
        cancel();
        attemptedWallet = false;
        sessionPassword = "";
        message = "";
        dismissRequested();
        requestTimer.restart();
    }

    Component.onCompleted: requestTimer.restart()
    Component.onDestruction: cancel()

    property Timer requestTimer: Timer {
        interval: 0
        onTriggered: root.requestPassword()
    }

    property Connections documentConnections: Connections {
        target: root.document
        function onPasswordRequired() {
            if (!root.submitting) {
                root.requestTimer.restart();
            }
        }
        function onSourceChanged() {
            // Also invalidates replies for replaced downloaded/filtered copies.
            root.cancel();
            root.attemptedWallet = false;
            root.dismissRequested();
            root.requestTimer.restart();
        }
    }

    property PdfPasswordStore wallet: PdfPasswordStore {
        source: root.source
        bookId: root.bookId
        window: root.window

        onLoaded: (password, found) => {
            root.readingWallet = false;
            if (found && password.length > 0) {
                root.submit(password, false);
            } else {
                root.passwordRequested("");
            }
        }
        onFailed: message => {
            if (root.readingWallet) {
                root.readingWallet = false;
                root.passwordRequested(message);
            } else {
                root.notification(i18nc("@info", "The PDF is open, but its password could not be saved in KWallet: %1", message));
            }
        }
    }
}
