// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick 2.15
import QtQuick.Controls 2.15 as QQC2
import QtQuick.Layouts 1.15
import Qt.labs.platform 1.1
import org.kde.kirigami 2.13 as Kirigami
import org.kde.arianna 1.0
import org.kde.kirigamiaddons.labs.components 1.0 as KirigamiComponents

Kirigami.ApplicationWindow {
    id: root

    property bool isLoading: true;

    title: i18n("Arianna")

    width: Kirigami.Units.gridUnit * 65
    height: Kirigami.Units.gridUnit * 45
    minimumWidth: Kirigami.Units.gridUnit * 20
    minimumHeight: Kirigami.Units.gridUnit * 30

    pageStack {
        defaultColumnWidth: Kirigami.Units.gridUnit * 30
        globalToolBar {
            canContainHandles: true
            style: Kirigami.ApplicationHeaderStyle.ToolBar
            showNavigationButtons: applicationWindow().pageStack.currentIndex > 0 ? Kirigami.ApplicationHeaderStyle.ShowBackButton : 0
        }
        initialPage: LibraryPage {
            bookListModel: root.bookListModel
            actions.main: addBookAction
        }
    }

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

    globalDrawer: Kirigami.OverlayDrawer {
        edge: Qt.application.layoutDirection === Qt.RightToLeft ? Qt.RightEdge : Qt.LeftEdge
        modal: Kirigami.Settings.isMobile || (applicationWindow().width < Kirigami.Units.gridUnit * 50 && !collapsed) // Only modal when not collapsed, otherwise collapsed won't show.
        z: modal ? Math.round(position * 10000000) : 100
        drawerOpen: !Kirigami.Settings.isMobile && enabled
        enabled: pageStack.currentItem.hideSidebar !== true && pageStack.layers.currentItem.hideSidebar !== true
        onEnabledChanged: drawerOpen = !Kirigami.Settings.isMobile && enabled
        width: Kirigami.Units.gridUnit * 16
        Behavior on width {
            NumberAnimation {
                duration: Kirigami.Units.longDuration
                easing.type: Easing.InOutQuad
            }
        }
        Kirigami.Theme.colorSet: Kirigami.Theme.Window

        handleClosedIcon.source: modal ? null : "sidebar-expand-left"
        handleOpenIcon.source: modal ? null : "sidebar-collapse-left"
        handleVisible: modal
        onModalChanged: if (!modal) {
            drawerOpen = true;
        }


        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: 0

        contentItem: ColumnLayout {
            spacing: 0

            QQC2.ToolBar {
                Layout.fillWidth: true
                Layout.preferredHeight: root.pageStack.globalToolBar.preferredHeight

                leftPadding: Kirigami.Units.largeSpacing
                rightPadding: Kirigami.Units.smallSpacing
                topPadding: Kirigami.Units.smallSpacing
                bottomPadding: Kirigami.Units.smallSpacing

                contentItem: KirigamiComponents.SearchPopupField {
                    id: searchField

                    spaceAvailableLeft: false
                    autoAccept: false

                    onTextChanged: searchFilterProxyModel.setFilterFixedString(text)

                    popupContentItem: ListView {
                        id: search

                        currentIndex: 0

                        Kirigami.Theme.colorSet: Kirigami.Theme.View
                        Kirigami.Theme.inherit: false

                        model: SortFilterProxyModel {
                            id: searchFilterProxyModel
                            sourceModel: root.bookListModel
                            filterRole: CategoryEntriesModel.TitleRole
                            filterCaseSensitivity: Qt.CaseInsensitive
                        }

                        delegate: QQC2.ItemDelegate {
                            id: searchDelegate

                            required property string title
                            required property string author
                            required property string filename
                            required property string locations
                            required property string currentLocation

                            leftInset: 1
                            rightInset: 1

                            highlighted: activeFocus

                            width: ListView.view.width
                            onClicked: {
                                Navigation.openBook(filename, locations, currentLocation);
                                searchField.popup.close();
                            }

                            contentItem: ColumnLayout {
                                QQC2.Label {
                                    Layout.fillWidth: true
                                    text: searchDelegate.title
                                    wrapMode: Text.WordWrap
                                }
                                QQC2.Label {
                                    Layout.fillWidth: true
                                    text: searchDelegate.author
                                    wrapMode: Text.WordWrap
                                    font: Kirigami.Theme.smallFont
                                }
                            }
                        }

                        Kirigami.PlaceholderMessage {
                            text: i18n("No search results")
                            visible: search.count === 0
                            icon.name: "system-search"
                            anchors.centerIn: parent
                            width: parent.width - Kirigami.Units.gridUnit * 4
                        }
                    }
                }
            }

            QQC2.ButtonGroup {
                id: placeGroup
            }

            QQC2.ScrollView {
                id: scrollView

                Layout.fillHeight: true
                Layout.fillWidth: true

                contentWidth: availableWidth

                component PlaceItem : Kirigami.BasicListItem {
                    id: item
                    signal triggered;
                    checkable: true
                    Layout.fillWidth: true
                    Keys.onDownPressed: nextItemInFocusChain().forceActiveFocus(Qt.TabFocusReason)
                    Keys.onUpPressed: nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocusReason)
                    Accessible.role: Accessible.MenuItem
                    highlighted: checked
                    onToggled: if (checked) {
                        item.triggered();
                    }
                }

                ColumnLayout {
                    spacing: 1
                    width: scrollView.width
                    PlaceItem {
                        text: i18nc("Switch to the listing page showing the most recently read books", "Home");
                        icon: "go-home";
                        checked: true
                        QQC2.ButtonGroup.group: placeGroup
                        onTriggered: Navigation.openLibrary(i18n("Home"), bookListModel, true)
                    }
                    PlaceItem {
                        text: i18nc("Switch to the listing page showing the most recently discovered books", "Recently Added Books");
                        icon: "appointment-new";
                        QQC2.ButtonGroup.group: placeGroup
                        onTriggered: Navigation.openLibrary(text, bookListModel.newlyAddedCategoryModel, true)
                    }
                    PlaceItem {
                        text: i18nc("Open a book from somewhere on disk (uses the open dialog, or a drilldown on touch devices)", "Open Other...");
                        icon: "document-open";
                        action: addBookAction
                        QQC2.ButtonGroup.group: null
                        checkable: false
                    }
                    Kirigami.ListSectionHeader {
                        text: i18nc("Heading for switching to listing page showing items grouped by some properties", "Group By")
                    }
                    PlaceItem {
                        text: i18nc("Switch to the listing page showing items grouped by author", "Author");
                        icon: "actor";
                        onTriggered: Navigation.openLibrary(text, bookListModel.authorCategoryModel, true)
                        QQC2.ButtonGroup.group: placeGroup
                    }
                    PlaceItem {
                        text: i18nc("Switch to the listing page showing items grouped by series", "Series");
                        icon: "edit-group";
                        onTriggered: Navigation.openLibrary(i18nc("Title of the page with books grouped by what series they are in", "Group by Series"), bookListModel.seriesCategoryModel, true)
                        QQC2.ButtonGroup.group: placeGroup
                    }
                    PlaceItem {
                        text: i18nc("Switch to the listing page showing items grouped by publisher", "Publisher");
                        icon: "view-media-publisher";
                        onTriggered: Navigation.openLibrary(text, bookListModel.publisherCategoryModel, true)
                        QQC2.ButtonGroup.group: placeGroup
                    }
                    PlaceItem {
                        text: i18nc("Switch to the listing page showing items grouped by genres", "Keywords");
                        icon: "tag";
                        onTriggered: Navigation.openLibrary(i18nc("Title of the page with books grouped by genres", "Group by Genres"), bookListModel.keywordCategoryModel, true)
                        QQC2.ButtonGroup.group: placeGroup
                    }
                }
            }

            Item {
                Layout.fillHeight: true
            }

            PlaceItem {
                text: i18nc("Open the settings page", "Settings");
                icon: "configure"
                onClicked: Navigation.openSettings()
                QQC2.ButtonGroup.group: placeGroup
                checkable: false
            }
        }
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

        function onOpenLibrary(title, model, replace) {
            if (!root.pageStack.currentItem.bookListModel) {
                root.pageStack.replace(Qt.resolvedUrl('./LibraryPage.qml'), {
                    title: title,
                    bookListModel: model,
                });
                root.pageStack.currentItem.title = title;
                root.pageStack.currentItem.bookListModel = model;
                return;
            };
            if (replace) {
                while (root.pageStack.depth > 1) {
                    root.pageStack.pop();
                };
                root.pageStack.currentItem.title = title;
                root.pageStack.currentItem.bookListModel = model;
                return;
            }

            root.pageStack.push(Qt.resolvedUrl('./LibraryPage.qml'), {
                title: title,
                bookListModel: model,
            });
        }

        function onOpenSettings() {
            pageStack.pushDialogLayer(Qt.resolvedUrl('./SettingsPage.qml'), {}, {
                title: i18n("Configure"),
            });
        }
    }

    Kirigami.Action {
        id: addBookAction
        text: i18nc("@action:button", "Add Book…")
        icon.name: "list-add"
        onTriggered: {
            const fileDialog = openFileDialog.createObject(QQC2.ApplicationWindow.overlay)
            fileDialog.accepted.connect(() => {
                const file = fileDialog.file;
                if (!file) {
                    return;
                }
                contentList.addFile(file)
            })
            fileDialog.open();
        }
    }

    Component {
        id: openFileDialog

        FileDialog {
            id: root
            parentWindow: applicationWindow()
            title: i18n("Please choose a file")
            nameFilters: [i18nc("Name filter for EPUB files", "eBook files (*.epub *.cb* *.fb2 *.fb2zip)")]
        }
    }
}
