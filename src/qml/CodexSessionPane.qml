// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.arianna

ColumnLayout {
    id: root
    property string prompt: ""
    property var readerContext: null
    signal contextSubmitted(var context, string question, string threadId)

    function connectToCodex() {
        CodexClient.connectToServer(Config.codexPath);
    }

    QQC2.Label {
        objectName: "codexConnectionStatus"
        Layout.fillWidth: true
        text: CodexClient.connected ? i18n("Connected to Codex %1", CodexClient.version)
            : CodexClient.state === CodexClient.Connecting ? i18n("Connecting to Codex…")
            : i18n("Codex is disconnected")
        color: CodexClient.connected ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.textColor
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
    }

    QQC2.Label {
        Layout.fillWidth: true
        text: root.readerContext !== null
            ? i18n("The displayed context and your question are sent to Codex. Read-only mode; requests for additional permissions are declined.")
            : i18n("Only the submitted text is sent to Codex. Read-only mode; requests for additional permissions are declined.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font: Kirigami.Theme.smallFont
    }

    QQC2.Label {
        objectName: "codexError"
        Layout.fillWidth: true
        visible: text.length > 0
        text: CodexClient.errorString
        textFormat: Text.PlainText
        color: Kirigami.Theme.negativeTextColor
        wrapMode: Text.Wrap
    }

    QQC2.ScrollView {
        id: outputScroll
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: Kirigami.Units.gridUnit * 4
        clip: true

        QQC2.TextArea {
            objectName: "codexOutput"
            width: outputScroll.availableWidth
            text: CodexClient.output
            textFormat: TextEdit.PlainText
            readOnly: true
            selectByMouse: true
            wrapMode: Text.Wrap
            placeholderText: i18n("Codex response")
        }
    }

    RowLayout {
        Layout.fillWidth: true

        QQC2.Button {
            objectName: "codexConnectButton"
            text: i18n("Connect")
            enabled: !CodexClient.connected && !CodexClient.busy
            onClicked: root.connectToCodex()
        }

        QQC2.Button {
            objectName: "codexSendButton"
            text: i18n("Send")
            icon.name: "document-send"
            enabled: CodexClient.state === CodexClient.Ready && root.prompt.trim().length > 0
            onClicked: {
                if (root.readerContext !== null) {
                    if (CodexClient.startReaderTurn(root.prompt, root.readerContext))
                        root.contextSubmitted(root.readerContext, root.prompt, CodexClient.threadId);
                } else {
                    CodexClient.startTurn(root.prompt);
                }
            }
        }

        QQC2.Button {
            objectName: "codexCancelButton"
            text: i18n("Cancel")
            icon.name: "process-stop"
            enabled: CodexClient.busy && CodexClient.state !== CodexClient.Stopping && CodexClient.state !== CodexClient.Cancelling
            onClicked: CodexClient.cancel()
        }

        QQC2.BusyIndicator {
            running: CodexClient.busy
            visible: running
            Layout.preferredWidth: Kirigami.Units.gridUnit * 2
            Layout.preferredHeight: Kirigami.Units.gridUnit * 2
        }
    }
}
