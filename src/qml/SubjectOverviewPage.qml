// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kitemmodels as KItemModels
import org.kde.kirigami as Kirigami
import org.kde.arianna

Kirigami.Page {
    id: root

    property CategoryEntriesModel bookListModel
    property var addBookAction
    property string searchText: applicationWindow().librarySearchText
    property string selectedSubject: ""
    property string selectedSubjectTitle: ""
    property var selectedSubjectModel: null
    readonly property bool subjectFeaturesReady: applicationWindow().bookListModel ? applicationWindow().bookListModel.subjectCategoryModelPopulated : false
    readonly property var subjectModel: applicationWindow().bookListModel ? applicationWindow().bookListModel.subjectCategoryModel : null
    readonly property bool compact: width < Kirigami.Units.gridUnit * 42
    readonly property bool searchActive: searchText.trim().length > 0
    readonly property real libraryZoomMinimum: 0.5
    readonly property real libraryZoomMaximum: 2.0
    readonly property real libraryZoomStep: 0.1
    readonly property real libraryZoomLevel: root.clampLibraryZoom(Config.libraryZoomLevel > 0 ? Config.libraryZoomLevel : 1.0)
    readonly property int defaultBookTileWidth: 170
    readonly property string epubTypeIconSource: "qrc:/qt/qml/org/kde/arianna/qml/icons/epub_icon.png"
    readonly property string pdfTypeIconSource: "qrc:/qt/qml/org/kde/arianna/qml/icons/pdf_icon.png"

    title: i18nc("@title:window", "Topics")
    padding: 0

    onSearchTextChanged: bookSortProxy.setFilterFixedString(searchText.trim())

    Component.onCompleted: {
        if (applicationWindow().bookListModel) {
            applicationWindow().bookListModel.populateSubjectCategoryModel();
        }
    }

    function selectSubject(subject, displayTitle, model) {
        root.selectedSubject = subject;
        root.selectedSubjectTitle = displayTitle;
        root.selectedSubjectModel = model;
        bookSortProxy.sourceModel = model;
        bookSortProxy.invalidate();
        bookSortProxy.setFilterFixedString(root.searchText.trim());
    }

    function clearSelection() {
        root.selectedSubject = "";
        root.selectedSubjectTitle = "";
        root.selectedSubjectModel = null;
        bookSortProxy.sourceModel = null;
    }

    function clampLibraryZoom(zoomLevel) {
        return Math.max(root.libraryZoomMinimum, Math.min(zoomLevel, root.libraryZoomMaximum));
    }

    function setLibraryZoom(zoomLevel) {
        const clampedZoomLevel = Math.round(root.clampLibraryZoom(zoomLevel) * 100) / 100;
        if (Math.abs(root.libraryZoomLevel - clampedZoomLevel) < 0.001) {
            return;
        }

        Config.libraryZoomLevel = clampedZoomLevel;
        Config.save();
    }

    function zoomLibraryFromWheel(event) {
        if (!(event.modifiers & Qt.ControlModifier)) {
            event.accepted = false;
            return false;
        }

        event.accepted = true;

        const angleDeltaY = event.angleDelta ? event.angleDelta.y : 0;
        const pixelDeltaY = event.pixelDelta ? event.pixelDelta.y : 0;
        const angleDeltaX = event.angleDelta ? event.angleDelta.x : 0;
        const pixelDeltaX = event.pixelDelta ? event.pixelDelta.x : 0;
        const delta = angleDeltaY !== 0 ? angleDeltaY : pixelDeltaY !== 0 ? pixelDeltaY : angleDeltaX !== 0 ? angleDeltaX : pixelDeltaX;
        if (delta === 0) {
            return true;
        }

        root.setLibraryZoom(root.libraryZoomLevel + (delta > 0 ? root.libraryZoomStep : -root.libraryZoomStep));
        return true;
    }

    function bookTileBaseWidth() {
        return Math.max(1, Math.round(root.defaultBookTileWidth * root.libraryZoomLevel));
    }

    function bookGridColumnCount(viewWidth) {
        const availableWidth = Math.max(1, viewWidth - Kirigami.Units.smallSpacing * 2);
        return Math.max(Math.floor(availableWidth / root.bookTileBaseWidth()), 1);
    }

    function bookGridCellWidth(viewWidth) {
        const availableWidth = Math.max(1, viewWidth - Kirigami.Units.smallSpacing * 2);
        return Math.floor(availableWidth / root.bookGridColumnCount(viewWidth));
    }

    function bookGridCellHeight(cellWidth) {
        const tileWidth = Kirigami.Settings.isMobile ? cellWidth : Math.min(root.bookTileBaseWidth(), cellWidth);
        return tileWidth + Kirigami.Units.gridUnit * 2 + Kirigami.Units.largeSpacing;
    }

    function bookTileWidth(viewWidth) {
        const cellWidth = root.bookGridCellWidth(viewWidth);
        return Kirigami.Settings.isMobile ? cellWidth : Math.min(root.bookTileBaseWidth(), cellWidth);
    }

    function typeIconSourceForFile(fileName, categoryEntriesModel) {
        if (categoryEntriesModel !== "") {
            return "";
        }

        const normalizedFileName = fileName ? fileName.toString().toLocaleLowerCase() : "";
        if (normalizedFileName.endsWith(".pdf")) {
            return root.pdfTypeIconSource;
        }
        if (normalizedFileName.endsWith(".epub")) {
            return root.epubTypeIconSource;
        }
        return "";
    }

    KItemModels.KSortFilterProxyModel {
        id: bookSortProxy

        sourceModel: root.selectedSubjectModel
        sortRoleName: "titleSort"
        sortOrder: Qt.AscendingOrder
        filterRole: CategoryEntriesModel.TitleRole
        filterCaseSensitivity: Qt.CaseInsensitive
        dynamicSortFilter: true

        Component.onCompleted: setFilterFixedString(root.searchText.trim())
    }

    Connections {
        target: root.subjectModel

        function onModelReset() {
            root.clearSelection();
        }
    }

    QQC2.SplitView {
        anchors.fill: parent
        orientation: root.compact ? Qt.Vertical : Qt.Horizontal

        QQC2.ScrollView {
            id: subjectPane

            QQC2.SplitView.preferredWidth: root.compact ? root.width : Kirigami.Units.gridUnit * 16
            QQC2.SplitView.preferredHeight: root.compact ? Kirigami.Units.gridUnit * 9 : root.height
            QQC2.SplitView.minimumWidth: root.compact ? Kirigami.Units.gridUnit * 12 : Kirigami.Units.gridUnit * 11
            QQC2.SplitView.minimumHeight: root.compact ? Kirigami.Units.gridUnit * 6 : Kirigami.Units.gridUnit * 12
            clip: true

            ListView {
                id: subjectList

                model: root.subjectModel
                reuseItems: true

                delegate: QQC2.ItemDelegate {
                    id: subjectDelegate

                    required property int index
                    required property string title
                    required property string localizedTitle
                    required property int categoryEntriesCount
                    required property var categoryEntriesModel

                    visible: categoryEntriesCount > 0
                    height: visible ? implicitHeight : 0
                    width: ListView.view.width
                    highlighted: root.selectedSubject === title
                    onClicked: {
                        subjectList.currentIndex = index;
                        root.selectSubject(title, localizedTitle, categoryEntriesModel);
                    }

                    Component.onCompleted: if (index === 0 && root.selectedSubject.length === 0 && categoryEntriesCount > 0) {
                        subjectList.currentIndex = index;
                        root.selectSubject(title, localizedTitle, categoryEntriesModel);
                    }

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.smallSpacing

                        QQC2.Label {
                            Layout.fillWidth: true
                            text: subjectDelegate.localizedTitle
                            elide: Text.ElideRight
                        }

                        QQC2.Label {
                            text: subjectDelegate.categoryEntriesCount
                            opacity: 0.7
                        }
                    }
                }

                Kirigami.PlaceholderMessage {
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.largeSpacing * 2
                    visible: subjectList.count === 0
                    icon.name: "tag-symbolic"
                    text: root.subjectFeaturesReady ? i18nc("@info placeholder", "No topics found") : i18nc("@info placeholder", "Loading topics…")
                    helpfulAction: root.subjectFeaturesReady && root.addBookAction ? root.addBookAction : null
                }
            }
        }

        Item {
            QQC2.SplitView.fillWidth: true
            QQC2.SplitView.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Kirigami.Heading {
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.largeSpacing
                    level: 2
                    text: root.selectedSubject.length > 0 ? i18nc("@title:group %1 is a topic and %2 is the number of books", "%1 (%2)", root.selectedSubjectTitle, root.selectedSubjectModel ? root.selectedSubjectModel.count : 0) : i18nc("@title:group", "Topics")
                    elide: Text.ElideRight
                }

                GridView {
                    id: subjectBooksView

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    leftMargin: Kirigami.Units.smallSpacing
                    rightMargin: Kirigami.Units.smallSpacing
                    topMargin: Kirigami.Units.smallSpacing
                    bottomMargin: Kirigami.Units.smallSpacing
                    model: bookSortProxy

                    cellWidth: {
                        return root.bookGridCellWidth(subjectBooksView.width);
                    }
                    cellHeight: root.bookGridCellHeight(cellWidth)
                    currentIndex: -1
                    reuseItems: true
                    activeFocusOnTab: true
                    keyNavigationEnabled: true

                    delegate: GridBrowserDelegate {
                        id: bookDelegate

                        required property string thumbnail
                        required property string title
                        required property string localizedTitle
                        required property string filename
                        required property var author
                        required property var entry
                        required property var locations
                        required property var currentLocation
                        required property bool passwordProtected
                        required property int categoryEntriesCount
                        required property var categoryEntriesModel

                        width: root.bookTileWidth(subjectBooksView.width)
                        height: subjectBooksView.cellHeight

                        imageUrl: categoryEntriesModel === "" ? ("file://" + thumbnail) : ""
                        iconName: if (categoryEntriesModel !== "") {
                            return thumbnail;
                        } else if (thumbnail === "") {
                            return "application-epub+zip";
                        } else {
                            return "";
                        }

                        mainText: bookDelegate.localizedTitle
                        secondaryText: author ? bookDelegate.author.join(", ") : ""
                        typeIconSource: root.typeIconSourceForFile(bookDelegate.filename, bookDelegate.categoryEntriesModel)
                        showPasswordBadge: bookDelegate.passwordProtected

                        onClicked: if (categoryEntriesModel) {
                            Navigation.openLibrary(localizedTitle, categoryEntriesModel, false);
                        } else {
                            Navigation.openBook(filename, locations, currentLocation, entry, false);
                        }

                        QQC2.ToolButton {
                            z: 20
                            anchors {
                                top: parent.top
                                right: parent.right
                                topMargin: Kirigami.Units.largeSpacing
                                rightMargin: Kirigami.Units.largeSpacing
                            }
                            width: Kirigami.Units.gridUnit * 2
                            height: Kirigami.Units.gridUnit * 2
                            display: QQC2.AbstractButton.IconOnly
                            icon.name: "documentinfo-symbolic"
                            text: i18nc("@action:button", "Book Details")
                            visible: !Kirigami.Settings.isMobile && bookDelegate.hovered && bookDelegate.categoryEntriesModel === ""
                            QQC2.ToolTip.text: text
                            QQC2.ToolTip.visible: hovered
                            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                            onClicked: applicationWindow().pageStack.pushDialogLayer(Qt.resolvedUrl("./BookDetailsPage.qml"), {
                                metadata: bookDelegate.entry,
                                bookListModel: applicationWindow().bookListModel
                            })
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        z: 100
                        acceptedButtons: Qt.NoButton
                        onWheel: event => root.zoomLibraryFromWheel(event)
                    }

                    Kirigami.PlaceholderMessage {
                        anchors.centerIn: parent
                        width: parent.width - Kirigami.Units.largeSpacing * 4
                        visible: subjectBooksView.count === 0
                        icon.name: "application-epub+zip"
                        text: root.searchActive ? i18nc("@info placeholder", "No books found") : i18nc("@info placeholder", "Select a topic")
                    }
                }
            }
        }
    }
}
