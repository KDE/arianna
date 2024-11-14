// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import Qt.labs.platform
import org.kde.arianna
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.delegates as Delegates
import org.kde.config as KConfig

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
            actions: addBookAction
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
        enabled: pageStack.currentItem && pageStack.currentItem.hideSidebar !== true && pageStack.layers.currentItem.hideSidebar !== true
        onEnabledChanged: drawerOpen = !Kirigami.Settings.isMobile && enabled
        width: Kirigami.Units.gridUnit * 16
        Behavior on width {
            NumberAnimation {
                duration: Kirigami.Units.longDuration
                easing.type: Easing.InOutQuad
            }
        }
        Kirigami.Theme.colorSet: Kirigami.Theme.View
        Kirigami.Theme.inherit: false

        handleClosedIcon.source: modal ? null : "sidebar-expand-left"
        handleOpenIcon.source: modal ? null : "sidebar-collapse-left"
        handleVisible: modal
        onModalChanged: if (!modal && pageStack.layers.currentItem.hideSidebar !== true) {
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

                leftPadding: Kirigami.Units.smallSpacing
                rightPadding: Kirigami.Units.smallSpacing
                topPadding: Kirigami.Units.smallSpacing
                bottomPadding: Kirigami.Units.smallSpacing

                contentItem: Kirigami.SearchField {
                    TapHandler {
                        onTapped: {
                            searchDialog.open();
                        }
                        acceptedButtons: Qt.RightButton | Qt.LeftButton
                    }
                    Keys.onPressed: (event) => {
                        if (event.key !== Qt.Key_Tab || event.key !== Qt.Key_Backtab) {
                            searchDialog.open();
                            searchDialog.text = text;
                        }
                    }
                    Keys.priority: Keys.AfterItem
                }

                Kirigami.SearchDialog {
                    id: searchDialog

                    parent: QQC2.Overlay.overlay

                    onTextChanged: {
                        searchFilterProxyModel.setFilterFixedString(text)
                    }

                    model: SortFilterProxyModel {
                        id: searchFilterProxyModel
                        sourceModel: root.bookListModel
                        filterRole: CategoryEntriesModel.TitleRole
                        filterCaseSensitivity: Qt.CaseInsensitive
                    }

                    delegate: Delegates.RoundedItemDelegate {
                        id: searchDelegate

                        required property int index
                        required property string title
                        required property string author
                        required property string filename
                        required property string locations
                        required property string currentLocation

                        highlighted: activeFocus

                        onClicked: {
                            Navigation.openBook(filename, locations, currentLocation);
                            searchDialog.close();
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

                    emptyText: i18n("No search results")
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
                topPadding: Kirigami.Units.smallSpacing / 2

                component PlaceItem : Delegates.RoundedItemDelegate {
                    id: item
                    signal triggered;
                    checkable: true
                    Layout.fillWidth: true
                    Keys.onDownPressed: nextItemInFocusChain().forceActiveFocus(Qt.TabFocusReason)
                    Keys.onUpPressed: nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocusReason)
                    Accessible.role: Accessible.MenuItem
                    highlighted: checked || activeFocus
                    onToggled: if (checked) {
                        item.triggered();
                    }
                }

                ColumnLayout {
                    spacing: 0
                    width: scrollView.width
                    PlaceItem {
                        id: goHomeButton
                        text: i18nc("Switch to the listing page showing the most recently read books", "Home");
                        icon.name: "go-home";
                        checked: true
                        QQC2.ButtonGroup.group: placeGroup
                        onTriggered: Navigation.openLibrary(i18n("Home"), bookListModel, true)
                    }
                    PlaceItem {
                        text: i18nc("Switch to the listing page showing the most recently discovered books", "Recently Added Books");
                        icon.name: "appointment-new";
                        QQC2.ButtonGroup.group: placeGroup
                        onTriggered: Navigation.openLibrary(text, bookListModel.newlyAddedCategoryModel, true)
                    }
                    PlaceItem {
                        text: i18nc("Open a book from somewhere on disk (uses the open dialog, or a drilldown on touch devices)", "Open Other...");
                        icon.name: "document-open";
                        action: addBookAction
                        QQC2.ButtonGroup.group: null
                        checkable: false
                    }

                    PlaceItem {
                        text: i18nc("Open the settings page", "Settings");
                        icon.name: "configure"
                        onClicked: Navigation.openSettings()
                        QQC2.ButtonGroup.group: placeGroup
                        checkable: false
                        Layout.bottomMargin: Kirigami.Units.smallSpacing / 2
                    }

                    Kirigami.ListSectionHeader {
                        text: i18nc("Heading for switching to listing page showing items grouped by some properties", "Group By")
                    }
                    PlaceItem {
                        text: i18nc("Switch to the listing page showing items grouped by author", "Author");
                        icon.name: "actor";
                        onTriggered: Navigation.openLibrary(text, bookListModel.authorCategoryModel, true)
                        QQC2.ButtonGroup.group: placeGroup
                    }
                    PlaceItem {
                        text: i18nc("Switch to the listing page showing items grouped by series", "Series");
                        icon.name: "edit-group";
                        onTriggered: Navigation.openLibrary(i18nc("Title of the page with books grouped by what series they are in", "Group by Series"), bookListModel.seriesCategoryModel, true)
                        QQC2.ButtonGroup.group: placeGroup
                    }
                    PlaceItem {
                        text: i18nc("Switch to the listing page showing items grouped by publisher", "Publisher");
                        icon.name: "view-media-publisher";
                        onTriggered: Navigation.openLibrary(text, bookListModel.publisherCategoryModel, true)
                        QQC2.ButtonGroup.group: placeGroup
                    }
                    PlaceItem {
                        text: i18nc("Switch to the listing page showing items grouped by genres", "Keywords");
                        icon.name: "tag";
                        onTriggered: Navigation.openLibrary(i18nc("Title of the page with books grouped by genres", "Group by Genres"), bookListModel.keywordCategoryModel, true)
                        QQC2.ButtonGroup.group: placeGroup
                    }
                }
            }

            Item {
                Layout.fillHeight: true
            }
        }
    }

    contextDrawer: TableOfContentDrawer {
        modal: !root.wideScreen || !enabled
        onEnabledChanged: drawerOpen = enabled && !modal
        enabled: root.pageStack.layers.depth > 1
        handleVisible: enabled
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
                    actions: addBookAction
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
                title: i18n("Settings"),
                width: Kirigami.Units.gridUnit * 12,
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

    KConfig.WindowStateSaver {
        configGroupName: "Main"
    }
}
