// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick 2.15
import QtQuick.Controls 2.15 as QQC2
import QtWebEngine 1.4
import QtWebChannel 1.4
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.13 as Kirigami
import org.kde.quickcharts 1.0 as Charts
import org.kde.arianna 1.0

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
        currentIndex: Kirigami.Settings.isMobile ? 0 : -1
        reuseItems: true
        activeFocusOnTab: true
        keyNavigationEnabled: true

        delegate: GridBrowserDelegate {
            id: bookDelegate

            required property string thumbnail
            required property string title
            required property string filename
            required property var author
            required property var locations
            required property var currentLocation
            required property int categoryEntriesCount
            required property var categoryEntriesModel

            width: Kirigami.Settings.isMobile ? contentDirectoryView.cellWidth : 170
            height: contentDirectoryView.cellHeight
            focus: true

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

            onOpen: if (categoryEntriesModel) {
                Navigation.openLibrary(title, categoryEntriesModel, false);
            } else {
                Navigation.openBook(filename, locations, currentLocation);
            }
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - (Kirigami.Units.largeSpacing * 4)
            visible: contentDirectoryView.count === 0
            icon.name: "application-epub+zip"
            text: i18nc("@info placeholder", "Add some books")
            helpfulAction: actions.main
        }
    }
}
