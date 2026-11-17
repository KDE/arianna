// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.arianna

ColumnLayout {
    id: root

    property bool libraryReady: true
    required property LibrarySearchModel searchModel
    signal searchRequested(string query)

    spacing: Kirigami.Units.smallSpacing

    function focusSearch() {
        searchField.forceActiveFocus();
    }

    Kirigami.SearchField {
        id: searchField
        objectName: "libraryFullTextSearchField"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        enabled: root.libraryReady && !root.searchModel.running
        placeholderText: i18nc("@info:placeholder", "Search all books…")
        Accessible.name: placeholderText
        autoAccept: false
        onAccepted: {
            if (root.libraryReady && !root.searchModel.running && text.trim().length > 0) {
                root.searchRequested(text);
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: root.searchModel.query.length > 0
        QQC2.Label {
            objectName: "libraryFullTextSearchProgress"
            Layout.fillWidth: true
            text: i18nc("@info:progress", "%1 / %2 books", root.searchModel.completed, root.searchModel.total)
        }
        QQC2.Button {
            objectName: "libraryFullTextSearchCancel"
            visible: root.searchModel.running
            enabled: !root.searchModel.cancelled
            icon.name: "process-stop"
            text: i18nc("@action:button", "Cancel")
            onClicked: root.searchModel.cancel()
        }
    }
    QQC2.ProgressBar {
        Layout.fillWidth: true
        visible: root.searchModel.running
        from: 0
        to: Math.max(1, root.searchModel.total)
        value: root.searchModel.completed
    }
    QQC2.Label {
        Layout.fillWidth: true
        visible: root.searchModel.query.length > 0
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: root.searchModel.cancelled
            ? (root.searchModel.running ? i18nc("@info", "Cancelling search…") : i18nc("@info", "Search cancelled. Results collected so far are shown."))
            : root.searchModel.running ? i18nc("@info", "Searching…")
            : root.searchModel.matchCount === 0 && root.searchModel.failedCount === 0 && root.searchModel.error.length === 0
                ? i18n("No search results") : i18nc("@info", "Search complete.")
    }
    QQC2.Label {
        Layout.fillWidth: true
        visible: root.searchModel.matchCount > 0
        text: i18ncp("@info", "%1 match", "%1 matches", root.searchModel.matchCount)
    }
    QQC2.Label {
        Layout.fillWidth: true
        visible: root.searchModel.failedCount > 0 || root.searchModel.error.length > 0
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: root.searchModel.error.length > 0 ? root.searchModel.error
            : i18ncp("@info", "%1 book could not be fully searched.", "%1 books could not be fully searched.", root.searchModel.failedCount)
    }

}
