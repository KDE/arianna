// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtWebEngine
import QtWebChannel
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.components as Components
import org.kde.quickcharts as Charts
import org.kde.arianna

Kirigami.ScrollablePage {
    id: root

    property CategoryEntriesModel bookListModel

    title: i18n("Library")

    GridView {
        id: contentDirectoryView

        leftMargin: Kirigami.Units.smallSpacing
        rightMargin: Kirigami.Units.smallSpacing
        topMargin: Kirigami.Units.smallSpacing
        bottomMargin: Kirigami.Units.smallSpacing

        model: root.bookListModel

        cellWidth: {
            const viewWidth = contentDirectoryView.width - Kirigami.Units.smallSpacing * 2;
            let columns = Math.max(Math.floor(viewWidth / 170), 2);
            return Math.floor(viewWidth / columns);
        }
        cellHeight: {
            if (Kirigami.Settings.isMobile) {
                return cellWidth + Kirigami.Units.gridUnit * 2 + Kirigami.Units.largeSpacing;
            } else {
                return 170 + Kirigami.Units.gridUnit * 2 + Kirigami.Units.largeSpacing
            }
        }
        currentIndex: -1
        reuseItems: true
        activeFocusOnTab: true
        keyNavigationEnabled: true

        delegate: GridBrowserDelegate {
            id: bookDelegate

            required property string thumbnail
            required property string title
            required property string filename
            required property var author
            required property var entry
            required property var locations
            required property var currentLocation
            required property int categoryEntriesCount
            required property var categoryEntriesModel

            width: Kirigami.Settings.isMobile ? contentDirectoryView.cellWidth : 170
            height: contentDirectoryView.cellHeight

            imageUrl: categoryEntriesModel === '' ? ('file://' + thumbnail) : ''
            iconName: if (categoryEntriesModel !== '') {
                return thumbnail;
            } else if (thumbnail === '') {
                return 'application-epub+zip';
            } else {
                return '';
            }

            mainText: bookDelegate.title
            secondaryText: author ? bookDelegate.author.join(', ') : ''

            onClicked: if (categoryEntriesModel) {
                Navigation.openLibrary(title, categoryEntriesModel, false);
            } else {
                Navigation.openBook(filename, locations, currentLocation, entry);
            }

            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: {
                    menu.entry = bookDelegate.entry;
                    menu.popup();
                }
            }
        }

        Components.ConvergentContextMenu {
            id: menu

            property var entry: null

            QQC2.Action {
                icon.name: 'documentinfo-symbolic'
                text: i18nc("@action:inmenu", "Book Details")
                onTriggered: applicationWindow().pageStack.pushDialogLayer(Qt.resolvedUrl("./BookDetailsPage.qml"), {
                    metadata: menu.entry,
                })
            }
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - (Kirigami.Units.largeSpacing * 4)
            visible: contentDirectoryView.count === 0
            icon.name: "application-epub+zip"
            text: i18nc("@info placeholder", "Add some books")
            helpfulAction: root.actions[0]
        }
    }
}
