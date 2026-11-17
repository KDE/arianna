// SPDX-License-Identifier: GPL-2.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.arianna

Kirigami.Page {
    id: root
    objectName: "bookImportPage"
    readonly property bool importPage: true
    required property QtObject importController
    required property ContentList balooModel
    readonly property bool libraryReady: importController.bookListModel.cacheLoaded
    readonly property string calibreFolder: String(Config.calibreLibraryFolder || "").trim()
    readonly property var sources: [
        { key: "files", label: i18nc("@title:tab", "File Dialog"), icon: "document-open" },
        { key: "baloo", label: i18nc("@title:tab", "Baloo"), icon: "edit-find" },
        { key: "calibre", label: i18nc("@title:tab", "Calibre"), icon: "document-import" }
    ]

    title: i18nc("@title:group", "Book Import")
    padding: Kirigami.Units.largeSpacing
    implicitWidth: Kirigami.Units.gridUnit * 56

    function showSource(source) {
        const index = sources.findIndex(item => item.key === source);
        if (index >= 0) {
            tabs.setCurrentIndex(index);
        }
    }

    header: QQC2.TabBar {
        id: tabs
        objectName: "bookImportTabs"
        implicitWidth: Kirigami.Units.gridUnit * 6
        Repeater {
            model: root.sources
            delegate: QQC2.TabButton {
                required property var modelData
                objectName: "bookImport-" + modelData.key
                text: modelData.label
                icon.name: modelData.icon
                width: implicitWidth
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            QQC2.ScrollView {
                id: filesView
                contentWidth: availableWidth
                ColumnLayout {
                    width: filesView.availableWidth
                    spacing: Kirigami.Units.largeSpacing
                    QQC2.Label {
                        Layout.fillWidth: true
                        text: i18n("Choose eBook files to add to your library.")
                        wrapMode: Text.Wrap
                    }
                    QQC2.Button {
                        objectName: "importFilesButton"
                        text: i18nc("@action:button", "Add Books…")
                        icon.name: "document-open"
                        enabled: root.libraryReady && !root.importController.calibreImportRunning
                        onClicked: root.importController.openAddBooksDialog()
                    }
                }
            }

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing
                QQC2.Label {
                    objectName: "balooImportSummary"
                    Layout.fillWidth: true
                    text: {
                        if (!root.balooModel.balooAvailable) {
                            return i18nc("@info", "Baloo is not available or file indexing is disabled. Use the File Dialog tab to add books.");
                        }
                        if (root.importController.balooImportRunning) {
                            return i18nc("@info", "Searching Baloo for books...");
                        }
                        if (root.balooModel.count > 0) {
                            return i18nc("@info %1 is a number", "Baloo found %1 possible imports. Import them into the library?", root.balooModel.count);
                        }
                        if (root.importController.balooImportSearched) {
                            return i18nc("@info", "No new books were found by Baloo.");
                        }
                        return i18n("Search the indexed folders for books that are not yet in the library.");
                    }
                    wrapMode: Text.Wrap
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: root.width < Kirigami.Units.gridUnit * 25 ? 1 : 2
                    QQC2.Button {
                        objectName: "searchBalooBooksButton"
                        text: i18nc("@action:button", "Search for Books")
                        icon.name: "edit-find"
                        enabled: root.libraryReady && root.balooModel.balooAvailable
                            && !root.importController.balooImportRunning && !root.importController.calibreImportRunning
                        onClicked: root.importController.startBalooImportSearch()
                    }
                    QQC2.Button {
                        objectName: "importBalooBooksButton"
                        text: i18nc("@action:button", "Import Books")
                        icon.name: "document-import"
                        enabled: root.libraryReady && root.balooModel.count > 0
                            && !root.importController.balooImportRunning && !root.importController.calibreImportRunning
                        onClicked: root.importController.importBalooSearchResults()
                    }
                }
                QQC2.BusyIndicator {
                    Layout.alignment: Qt.AlignHCenter
                    visible: root.importController.balooImportRunning
                    running: visible
                }
                QQC2.ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    ListView {
                        id: balooResults
                        objectName: "balooImportResultsView"
                        model: root.balooModel
                        reuseItems: true
                        delegate: QQC2.ItemDelegate {
                            required property string filename
                            required property url filePath
                            width: ListView.view.width
                            text: filename
                            icon.name: String(filePath).toLowerCase().endsWith(".pdf") ? "application-pdf" : "application-epub+zip"
                            QQC2.ToolTip.visible: hovered
                            QQC2.ToolTip.text: String(filePath)
                            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                        }
                    }
                }
            }

            QQC2.ScrollView {
                id: calibreView
                contentWidth: availableWidth
                ColumnLayout {
                    width: calibreView.availableWidth
                    spacing: Kirigami.Units.largeSpacing
                    Kirigami.Heading {
                        level: 2
                        Layout.fillWidth: true
                        text: i18n("Calibre Library Folder")
                        wrapMode: Text.Wrap
                    }
                    QQC2.Label {
                        objectName: "calibreImportFolder"
                        Layout.fillWidth: true
                        text: root.calibreFolder || i18n("No Calibre library folder")
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        visible: root.calibreFolder.length === 0
                        text: i18n("Set the Calibre library folder in Settings first.")
                        wrapMode: Text.Wrap
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: root.width < Kirigami.Units.gridUnit * 25 ? 1 : 2
                        QQC2.Button {
                            objectName: "importCalibreBooksButton"
                            text: i18nc("@action:button", "Import Books from Calibre")
                            icon.name: "document-import"
                            enabled: root.libraryReady && !root.importController.calibreImportRunning
                                && !root.importController.balooImportRunning
                            onClicked: root.importController.startCalibreLibraryImport()
                        }
                        QQC2.Button {
                            text: i18nc("Open the settings page", "Settings")
                            icon.name: "configure"
                            onClicked: Navigation.openSettings()
                        }
                    }
                    QQC2.BusyIndicator {
                        Layout.alignment: Qt.AlignHCenter
                        visible: root.importController.calibreImportRunning
                        running: visible
                    }
                }
            }
        }

        QQC2.Label {
            objectName: "bookImportStatus"
            Layout.fillWidth: true
            visible: text.length > 0
            text: root.importController.bookImportStatus
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
    }
}
