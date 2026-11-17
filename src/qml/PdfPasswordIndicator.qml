// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

import QtQuick
import org.kde.arianna

QtObject {
    id: root

    property bool enabled: false
    property string fileName: ""
    property string bookId: ""
    property Window window: null
    readonly property bool unlocked: enabled && wallet.passwordValid
    readonly property string description: unlocked
        ? i18n("PDF: saved KWallet password is valid")
        : wallet.busy ? i18n("PDF: checking saved KWallet password…")
        : i18n("PDF: no validated KWallet password available")
    property bool allowWalletOpen: false

    function schedule(allowOpen) {
        if (!enabled || !fileName) {
            checkTimer.stop();
            allowWalletOpen = false;
            return;
        }
        allowWalletOpen = allowWalletOpen || allowOpen;
        checkTimer.restart();
    }

    onEnabledChanged: schedule(true)
    onFileNameChanged: schedule(true)
    onBookIdChanged: schedule(true)
    Component.onCompleted: schedule(true)

    property Timer checkTimer: Timer {
        interval: 150
        onTriggered: {
            const allowOpen = root.allowWalletOpen;
            root.allowWalletOpen = false;
            wallet.check(allowOpen);
        }
    }

    property PdfPasswordStore wallet: PdfPasswordStore {
        objectName: "pdfCoverPasswordStore"
        fileName: root.enabled ? root.fileName : ""
        bookId: root.bookId
        window: root.window
        onInvalidated: refresh => {
            if (refresh) {
                root.schedule(false);
            } else {
                // Closing KWallet clears the icon without opening it again.
                checkTimer.stop();
                root.allowWalletOpen = false;
            }
        }
    }

    property Connections applicationState: Connections {
        target: Qt.application
        function onStateChanged() {
            if (Qt.application.state === Qt.ApplicationActive) root.schedule(false);
        }
    }
}
