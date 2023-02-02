// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick 2.15
import QtQuick.Controls 2.15 as QQC2
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.13 as Kirigami
import org.kde.arianna 1.0

Kirigami.ApplicationWindow {
    id: root

    title: i18n("Arianna")

    width: Kirigami.Units.gridUnit * 30
    height: Kirigami.Units.gridUnit * 40
    minimumWidth: Kirigami.Units.gridUnit * 20
    minimumHeight: Kirigami.Units.gridUnit * 30

    property bool isLoading: true;

    readonly property BookListModel bookListModel: BookListModel {
        contentModel: ContentList {
            id: contentList
            autoSearch: false

            onSearchStarted: root.isLoading = true
            onSearchCompleted: root.isLoading = false

            ContentQuery {
                type: ContentQuery.Epub
                locations: Config.bookLocations
            }
        }
        onCacheLoadedChanged: {
            if (!cacheLoaded) {
                return;
            }
            contentModel.setKnownFiles(knownBookFiles());
            contentModel.startSearch()
        }
    }

    pageStack.initialPage: LibraryPage {
        bookListModel: root.bookListModel
    }

    Connections {
        target: Navigation

        function onOpenBook(filename, locations, currentLocation) {
            const epubViewer = root.pageStack.layers.push('./EpubViewerPage.qml', {
                url: 'file://' + filename,
                filename: filename,
                locations: locations,
                currentLocation: currentLocation,
            });

            epubViewer.relocated.connect((newLocation, newProgress) => {
                bookListModel.setBookData(filename, 'currentLocation', newLocation);
                bookListModel.setBookData(filename, 'currentProgress', newProgress);
            });

            epubViewer.locationsLoaded.connect((locations) => {
                bookListModel.setBookData(filename, 'locations', locations);
            });

            epubViewer.bookReady.connect((title) => {
                root.title = title;
            });

            epubViewer.bookClosed.connect(() => {
               root.title = i18n("Arianna");
            });
        }
    }
}
