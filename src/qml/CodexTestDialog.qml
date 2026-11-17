// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.arianna

Kirigami.Dialog {
    id: root
    objectName: "codexTestDialog"
    title: i18n("Test Codex connection")
    modal: true
    standardButtons: QQC2.Dialog.Close
    width: Math.min(Kirigami.Units.gridUnit * 38, parent ? parent.width - Kirigami.Units.gridUnit * 2 : 760)
    height: Math.min(Kirigami.Units.gridUnit * 32, parent ? parent.height - Kirigami.Units.gridUnit * 2 : 640)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    onOpened: session.connectToCodex()
    onClosed: CodexClient.disconnectFromServer()
    Component.onDestruction: {
        if (visible) CodexClient.disconnectFromServer();
    }

    contentItem: ColumnLayout {
        QQC2.ScrollView {
            id: inputScroll
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 7
            clip: true
            QQC2.TextArea {
                id: input
                objectName: "codexInput"
                width: inputScroll.availableWidth
                selectByMouse: true
                wrapMode: Text.Wrap
                placeholderText: i18n("Enter a question or task")
            }
        }
        CodexSessionPane {
            id: session
            Layout.fillWidth: true
            Layout.fillHeight: true
            prompt: input.text
        }
    }
}
