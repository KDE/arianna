// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtWebChannel
import QtQuick.Layouts
import org.kde.kitemmodels as KItemModels
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.components as Components
import org.kde.quickcharts as Charts
import org.kde.arianna

Kirigami.Page {
    id: root

    property CategoryEntriesModel bookListModel
    property var addBookAction
    property string pageTitle: i18n("Library")
    property string searchText: applicationWindow().librarySearchText
    property bool showSubjectFilter: false
    property string selectedSubject: ""
    property var selectedSubjectModel: null
    property string hoveredSubjectBook: ""
    property var hoveredSubjects: []
    property string pinnedSubjectBook: ""
    property string pinnedSubjectBookTitle: ""
    property var pinnedSubjects: []
    property var subjectChips: []
    property var subjectChipGroups: []
    property var bookGroups: []
    property var bookGroupRows: []
    property bool groupingEnabled: false
    property bool suppressNextBookClick: false
    property bool componentReady: false
    property bool pendingViewRefresh: false
    property bool pendingSubjectRefresh: false
    property bool librarySearchFieldActiveFocus: false
    property int pendingBookGroupPreparationDelay: -1
    property bool viewRefreshScheduled: false
    property bool subjectRefreshScheduled: false
    property bool bookGroupRowsRefreshScheduled: false
    property var pendingOverviewPositionRestore: null
    property int pendingOverviewPositionRestoreAttempts: 0
    property bool showBookCount: root.bookListModel === applicationWindow().bookListModel
    readonly property bool libraryPage: true
    readonly property bool refreshActive: root.visible && applicationWindow().pageStack && applicationWindow().pageStack.currentItem === root
    readonly property bool searchActive: searchText.trim().length > 0
    readonly property string addBooksActionText: root.addBookAction && root.addBookAction.text.length > 0 ? root.addBookAction.text : i18nc("@action:button", "Add Books…")
    readonly property string addBooksActionIconName: root.addBookAction && root.addBookAction.icon.name.length > 0 ? root.addBookAction.icon.name : "list-add"
    readonly property bool addBooksActionEnabled: root.addBookAction ? root.addBookAction.enabled : true
    readonly property int totalBookCount: applicationWindow().bookListModel ? applicationWindow().bookListModel.count : 0
    readonly property bool subjectFeaturesReady: applicationWindow().bookListModel ? applicationWindow().bookListModel.subjectCategoryModelPopulated : false
    readonly property var subjectModel: applicationWindow().bookListModel ? applicationWindow().bookListModel.subjectCategoryModel : null
    readonly property bool subjectFilterAvailable: root.showBookCount && root.subjectModel && root.subjectModel.count > 0
    readonly property bool subjectFilterVisible: root.showSubjectFilter && root.showBookCount && (!root.subjectFeaturesReady || root.subjectFilterAvailable)
    readonly property bool subjectAssignmentActive: root.pinnedSubjectBook.length > 0
    readonly property bool groupingAvailable: root.showBookCount && (sortProxy.sortRoleName === "mainSubjectSort" || sortProxy.sortRoleName === "titleSort" || sortProxy.sortRoleName === "authorSort" || sortProxy.sortRoleName === "typeSort" || sortProxy.sortRoleName === "lastOpenedTime")
    readonly property bool mainSubjectGroupingVisible: root.groupingEnabled && root.showBookCount && sortProxy.sortRoleName === "mainSubjectSort"
    readonly property bool titleGroupingVisible: root.groupingEnabled && root.showBookCount && sortProxy.sortRoleName === "titleSort"
    readonly property bool authorGroupingVisible: root.groupingEnabled && root.showBookCount && sortProxy.sortRoleName === "authorSort"
    readonly property bool typeGroupingVisible: root.groupingEnabled && root.showBookCount && sortProxy.sortRoleName === "typeSort"
    readonly property bool lastOpenedGroupingVisible: root.groupingEnabled && root.showBookCount && sortProxy.sortRoleName === "lastOpenedTime"
    readonly property bool bookGroupingVisible: root.mainSubjectGroupingVisible || root.titleGroupingVisible || root.authorGroupingVisible || root.typeGroupingVisible || root.lastOpenedGroupingVisible
    readonly property bool authorCategoryPage: applicationWindow().bookListModel && root.bookListModel === applicationWindow().bookListModel.authorCategoryModel
    readonly property real libraryZoomMinimum: 0.5
    readonly property real libraryZoomMaximum: 2.0
    readonly property real libraryZoomStep: 0.1
    readonly property real libraryZoomLevel: root.clampLibraryZoom(Config.libraryZoomLevel > 0 ? Config.libraryZoomLevel : 1.0)
    readonly property int defaultBookTileWidth: 170
    readonly property string epubTypeIconSource: "qrc:/qt/qml/org/kde/arianna/qml/icons/epub_icon.png"
    readonly property string pdfTypeIconSource: "qrc:/qt/qml/org/kde/arianna/qml/icons/pdf_icon.png"

    title: if (!showBookCount) {
        return pageTitle;
    } else if (subjectFilterAvailable && showSubjectFilter) {
        return i18nc("@title:window, %1 is the page title, %2 is the number of books, %3 is the number of topics", "%1 (%2/%3)", pageTitle, totalBookCount, subjectModel.count);
    } else {
        return i18nc("@title:window, %1 is the page title and %2 is the number of books", "%1 (%2)", pageTitle, totalBookCount);
    }
    padding: 0
    focus: true

    Keys.onPressed: event => {
        if (root.handleAuthorJumpKey(event)) {
            event.accepted = true;
        }
    }

    titleDelegate: Item {
        id: libraryHeaderTitleDelegate

        readonly property int headerSpacing: Kirigami.Units.largeSpacing
        readonly property int searchMinimumWidth: Kirigami.Units.gridUnit * 8
        readonly property int searchMaximumWidth: Kirigami.Units.gridUnit * 18
        readonly property int searchAvailableWidth: Math.max(0, width - headerSpacing * 2)

        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.preferredWidth: root.width
        implicitWidth: Kirigami.Units.gridUnit * 32
        implicitHeight: Math.max(libraryHeaderTitle.implicitHeight, librarySearchField.implicitHeight)

        Kirigami.Heading {
            id: libraryHeaderTitle

            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(0, Math.min(implicitWidth, librarySearchField.visible ? librarySearchField.x - libraryHeaderTitleDelegate.headerSpacing : parent.width))
            maximumLineCount: 1
            elide: Text.ElideRight
            text: root.title
            textFormat: Text.PlainText
        }

        Kirigami.SearchField {
            id: librarySearchField

            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            visible: libraryHeaderTitleDelegate.searchAvailableWidth >= libraryHeaderTitleDelegate.searchMinimumWidth
            width: Math.min(libraryHeaderTitleDelegate.searchMaximumWidth, libraryHeaderTitleDelegate.searchAvailableWidth)
            placeholderText: i18nc("@info:placeholder", "Search by title/author...")
            text: applicationWindow().librarySearchText
            onActiveFocusChanged: root.librarySearchFieldActiveFocus = activeFocus
            onTextChanged: {
                if (applicationWindow().librarySearchText !== text) {
                    applicationWindow().librarySearchText = text;
                }
            }
            Component.onCompleted: root.librarySearchFieldActiveFocus = activeFocus
            Component.onDestruction: if (root.librarySearchFieldActiveFocus) {
                root.librarySearchFieldActiveFocus = false;
            }
        }
    }

    onSearchTextChanged: scheduleViewRefresh()
    onGroupingEnabledChanged: {
        scheduleViewRefresh();
        scheduleBookGroupPreparation();
    }
    onMainSubjectGroupingVisibleChanged: scheduleViewRefresh()
    onTitleGroupingVisibleChanged: scheduleViewRefresh()
    onAuthorGroupingVisibleChanged: scheduleViewRefresh()
    onTypeGroupingVisibleChanged: scheduleViewRefresh()
    onLastOpenedGroupingVisibleChanged: scheduleViewRefresh()
    onBookGroupsChanged: scheduleBookGroupRowsRefresh()
    onLibraryZoomLevelChanged: scheduleBookGroupRowsRefresh()
    onRefreshActiveChanged: if (componentReady) {
        handleRefreshActiveChanged();
    }
    onSubjectFeaturesReadyChanged: {
        scheduleSubjectRefresh();
    }
    onBookListModelChanged: {
        clearSubjectFilter();
        clearHoveredSubjects(root.hoveredSubjectBook);
        clearPinnedSubjectBook();
        scheduleSubjectRefresh();
        scheduleSubjectPopulation();
        scheduleViewRefresh();
    }

    function selectSubjectFilter(subject, model) {
        root.selectedSubject = subject;
        root.selectedSubjectModel = model;
        root.scheduleViewRefresh();
    }

    function clearSubjectFilter() {
        root.selectedSubject = "";
        root.selectedSubjectModel = null;
    }

    function refreshSubjectChips() {
        const chips = root.subjectModel ? root.subjectModel.flatCategoryEntries() : [];
        root.subjectChips = chips;
        root.subjectChipGroups = root.groupedSubjectChips(chips);
    }

    function subjectLevelTitle(level) {
        if (level <= 1) {
            return i18nc("@label subject hierarchy level", "Main Topic");
        }

        return i18nc("@label subject hierarchy level, %1 is the hierarchy depth below the main topic", "Subtopic %1", level - 1);
    }

    function groupedSubjectChips(chips) {
        const groups = [];
        const groupsByLevel = {};

        for (let i = 0; i < chips.length; ++i) {
            const chip = chips[i] || {};
            const count = Number(chip.categoryEntriesCount || 0);
            if (count <= 0) {
                continue;
            }

            const level = Math.max(1, Number(chip.subjectLevel || 1));
            if (!groupsByLevel[level]) {
                groupsByLevel[level] = {
                    level: level,
                    title: root.subjectLevelTitle(level),
                    chips: []
                };
                groups.push(groupsByLevel[level]);
            }

            groupsByLevel[level].chips.push(chip);
        }

        groups.sort((first, second) => first.level - second.level);
        return groups;
    }

    function bookSecondaryText(mainSubject, author, isBook) {
        const authors = author ? author.join(", ") : "";
        if (isBook && sortProxy.sortRoleName === "authorSort") {
            return mainSubject && mainSubject.length > 0 ? mainSubject : authors;
        }

        if (!isBook || sortProxy.sortRoleName !== "mainSubjectSort") {
            return authors;
        }

        if (!mainSubject || mainSubject.length === 0) {
            return authors;
        }

        if (authors.length === 0) {
            return mainSubject;
        }

        return i18nc("@info book subtitle, %1 is the main topic and %2 is the author list", "%1 - %2", mainSubject, authors);
    }

    function authorTitleWithCount(title, bookCount) {
        const count = Number(bookCount || 0);
        if (count > 3) {
            return i18nc("@title:section author name with book count, %1 is the author name and %2 is the number of books", "%1 (%2)", title, count);
        }

        return title;
    }

    function bookGroupTitle(group) {
        const title = group && group.title ? group.title : "";
        if (root.authorGroupingVisible) {
            return root.authorTitleWithCount(title, group.bookCount);
        }

        return title;
    }

    function groupedBookColumns(viewWidth) {
        const availableWidth = Math.max(1, viewWidth - Kirigami.Units.smallSpacing * 2);
        const tileWidth = root.bookTileWidth(viewWidth);
        return Math.max(1, Math.floor((availableWidth + Kirigami.Units.smallSpacing) / (tileWidth + Kirigami.Units.smallSpacing)));
    }

    function rebuildBookGroupRows() {
        if (!root.bookGroupingVisible || root.bookGroups.length === 0) {
            root.bookGroupRows = [];
            return;
        }

        const viewWidth = Math.max(1, mainSubjectGroupContainer.width > 0 ? mainSubjectGroupContainer.width : root.width);
        const columns = root.groupedBookColumns(viewWidth);
        const rows = [];

        for (let groupIndex = 0; groupIndex < root.bookGroups.length; ++groupIndex) {
            const group = root.bookGroups[groupIndex] || {};
            const books = group.books || [];
            rows.push({
                type: "header",
                group,
                groupIndex,
                title: root.bookGroupTitle(group),
                bookCount: Number(group.bookCount || books.length || 0)
            });

            for (let firstBookIndex = 0; firstBookIndex < books.length; firstBookIndex += columns) {
                const rowBooks = [];
                for (let bookIndex = firstBookIndex; bookIndex < Math.min(firstBookIndex + columns, books.length); ++bookIndex) {
                    rowBooks.push(books[bookIndex]);
                }
                rows.push({
                    type: "books",
                    group,
                    groupIndex,
                    firstBookIndex,
                    books: rowBooks
                });
            }

            if (groupIndex < root.bookGroups.length - 1) {
                rows.push({
                    type: "spacer"
                });
            }
        }

        if (rows.length > 0) {
            rows.push({
                type: "spacer"
            });
        }
        root.bookGroupRows = rows;
    }

    function scheduleBookGroupRowsRefresh() {
        if (root.bookGroupRowsRefreshScheduled) {
            return;
        }

        root.bookGroupRowsRefreshScheduled = true;
        Qt.callLater(() => {
            root.bookGroupRowsRefreshScheduled = false;
            root.rebuildBookGroupRows();
        });
    }

    function bookTileMainText(localizedTitle, categoryEntriesCount, categoryEntriesModel) {
        const title = localizedTitle || "";
        if (root.authorCategoryPage && categoryEntriesModel !== "" && Number(categoryEntriesCount || 0) > 3) {
            return root.authorTitleWithCount(title, categoryEntriesCount);
        }

        return title;
    }

    function foldedAuthorJumpText(value) {
        let text = String(value || "").trim().toLocaleLowerCase();
        if (text.normalize) {
            text = text.normalize("NFD").replace(/[\u0300-\u036f]/g, "");
        }

        return text.replace(/ß/g, "ss");
    }

    function authorJumpLetterFromEvent(event) {
        if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) {
            return "";
        }

        let text = event.text || "";
        if (text.length === 0 && event.key >= Qt.Key_A && event.key <= Qt.Key_Z) {
            text = String.fromCharCode("a".charCodeAt(0) + event.key - Qt.Key_A);
        }
        if (text.length !== 1) {
            return "";
        }

        const folded = root.foldedAuthorJumpText(text);
        if (folded.length === 0) {
            return "";
        }

        const letter = folded.charAt(0);
        return /^[a-z]$/.test(letter) ? letter : "";
    }

    function textStartsWithAuthorJumpLetter(value, letter) {
        return root.foldedAuthorJumpText(value).startsWith(letter);
    }

    function authorListStartsWithJumpLetter(authors, letter) {
        if (!authors) {
            return false;
        }

        const authorList = Array.isArray(authors) ? authors : String(authors).split(",");
        for (let i = 0; i < authorList.length; ++i) {
            if (root.textStartsWithAuthorJumpLetter(authorList[i], letter)) {
                return true;
            }
        }

        return false;
    }

    function roleData(model, row, role) {
        if (!model || typeof model.index !== "function" || typeof model.data !== "function") {
            return undefined;
        }

        return model.data(model.index(row, 0), role);
    }

    function findAuthorGridIndex(letter) {
        for (let i = 0; i < contentDirectoryView.count; ++i) {
            if (root.authorCategoryPage) {
                const title = root.roleData(sortProxy, i, CategoryEntriesModel.LocalizedTitleRole)
                    || root.roleData(sortProxy, i, CategoryEntriesModel.TitleRole);
                const categoryEntriesCount = Number(root.roleData(sortProxy, i, CategoryEntriesModel.CategoryEntryCountRole) || 0);
                const categoryEntriesModel = root.roleData(sortProxy, i, CategoryEntriesModel.CategoryEntriesModelRole);
                if (categoryEntriesModel && categoryEntriesCount > 0 && root.textStartsWithAuthorJumpLetter(title, letter)) {
                    return i;
                }
            }

            const authors = root.roleData(sortProxy, i, CategoryEntriesModel.AuthorRole);
            if (root.authorListStartsWithJumpLetter(authors, letter)) {
                return i;
            }
        }

        return -1;
    }

    function findAuthorGroupIndex(letter) {
        for (let i = 0; i < root.bookGroups.length; ++i) {
            const group = root.bookGroups[i] || {};
            if (root.textStartsWithAuthorJumpLetter(group.title || "", letter)) {
                return i;
            }
        }

        return -1;
    }

    function scrollToAuthorGridIndex(index) {
        if (index < 0) {
            return false;
        }

        contentDirectoryView.currentIndex = index;
        contentDirectoryView.positionViewAtIndex(index, GridView.Beginning);
        contentDirectoryView.forceActiveFocus();
        return true;
    }

    function bookGroupHeaderRowIndex(groupIndex) {
        for (let i = 0; i < root.bookGroupRows.length; ++i) {
            const row = root.bookGroupRows[i] || {};
            if (row.type === "header" && row.groupIndex === groupIndex) {
                return i;
            }
        }

        return -1;
    }

    function scrollToAuthorGroupIndex(index) {
        if (index < 0) {
            return false;
        }

        const rowIndex = root.bookGroupHeaderRowIndex(index);
        if (rowIndex < 0) {
            return false;
        }

        groupedBookListView.currentIndex = rowIndex;
        groupedBookListView.positionViewAtIndex(rowIndex, ListView.Beginning);
        mainSubjectGroupContainer.forceActiveFocus();
        return true;
    }

    function jumpToAuthorLetter(letter) {
        if (root.authorGroupingVisible) {
            return root.scrollToAuthorGroupIndex(root.findAuthorGroupIndex(letter));
        }

        if (!root.bookGroupingVisible && (sortProxy.sortRoleName === "authorSort" || root.authorCategoryPage)) {
            return root.scrollToAuthorGridIndex(root.findAuthorGridIndex(letter));
        }

        return false;
    }

    function handleAuthorJumpKey(event) {
        const letter = root.authorJumpLetterFromEvent(event);
        if (letter.length === 0) {
            return false;
        }

        return root.jumpToAuthorLetter(letter);
    }

    function currentEntriesModel() {
        return root.subjectFilterVisible && root.selectedSubjectModel ? root.selectedSubjectModel : root.bookListModel;
    }

    function refreshBookGroups() {
        const model = root.currentEntriesModel();
        if (!root.bookGroupingVisible || !model) {
            root.bookGroups = [];
            return;
        }

        if (root.titleGroupingVisible) {
            root.bookGroups = model.titleBookGroups(root.searchText.trim());
        } else if (root.authorGroupingVisible) {
            root.bookGroups = model.authorBookGroups(root.searchText.trim());
        } else if (root.typeGroupingVisible) {
            root.bookGroups = model.typeBookGroups(root.searchText.trim());
        } else if (root.lastOpenedGroupingVisible) {
            root.bookGroups = model.lastOpenedBookGroups(root.searchText.trim());
        } else {
            root.bookGroups = model.mainSubjectBookGroups(root.searchText.trim());
        }
    }

    function prepareCurrentBookGroups() {
        const model = root.currentEntriesModel();
        if (!model || root.groupingEnabled || !root.groupingAvailable || root.searchActive) {
            return;
        }

        const libraryModel = applicationWindow().bookListModel;
        if (!libraryModel || !libraryModel.cacheLoaded) {
            root.scheduleBookGroupPreparation(500);
            return;
        }

        const filterText = root.searchText.trim();
        if (sortProxy.sortRoleName === "titleSort") {
            model.titleBookGroups(filterText);
        } else if (sortProxy.sortRoleName === "authorSort") {
            model.authorBookGroups(filterText);
        } else if (sortProxy.sortRoleName === "typeSort") {
            model.typeBookGroups(filterText);
        } else if (sortProxy.sortRoleName === "lastOpenedTime") {
            model.lastOpenedBookGroups(filterText);
        } else {
            model.mainSubjectBookGroups(filterText);
        }
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

    function bookTileHeight(viewWidth) {
        return root.bookGridCellHeight(root.bookGridCellWidth(viewWidth));
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

    function normalizedSubjectText(subject) {
        return subject ? subject.toString().trim().toLocaleLowerCase() : "";
    }

    function subjectList(subjects) {
        if (!subjects) {
            return [];
        }
        const values = typeof subjects === "string" ? subjects.split(",") : subjects;
        const normalized = [];
        for (let i = 0; i < values.length; ++i) {
            const subject = values[i].toString().trim();
            if (subject.length > 0) {
                normalized.push(subject);
            }
        }
        return normalized;
    }

    function setHoveredSubjects(fileName, subjects) {
        if (!fileName || fileName.length === 0) {
            return;
        }
        root.hoveredSubjectBook = fileName;
        root.hoveredSubjects = root.subjectList(subjects);
    }

    function clearHoveredSubjects(fileName) {
        if (root.hoveredSubjectBook !== fileName) {
            return;
        }
        root.hoveredSubjectBook = "";
        root.hoveredSubjects = [];
    }

    function setPinnedSubjectBook(fileName, title, subjects) {
        if (!fileName || fileName.length === 0 || !root.showBookCount) {
            return;
        }
        root.populateSubjectModelNow();
        root.showSubjectFilter = true;
        root.pinnedSubjectBook = fileName;
        root.pinnedSubjectBookTitle = title || "";
        root.pinnedSubjects = root.subjectList(subjects);
        root.scheduleViewRefresh();
    }

    function clearPinnedSubjectBook() {
        root.pinnedSubjectBook = "";
        root.pinnedSubjectBookTitle = "";
        root.pinnedSubjects = [];
    }

    function togglePinnedSubjectBook(fileName, title, subjects) {
        if (root.pinnedSubjectBook === fileName) {
            root.clearPinnedSubjectBook();
            root.clearSubjectFilter();
            root.clearHoveredSubjects(root.hoveredSubjectBook);
            root.showSubjectFilter = false;
            root.scheduleViewRefresh();
        } else {
            root.setPinnedSubjectBook(fileName, title, subjects);
        }
    }

    function activeSubjectList() {
        return root.subjectAssignmentActive ? root.pinnedSubjects : root.hoveredSubjects;
    }

    function subjectMatches(bookSubject, chipSubject) {
        const normalizedBookSubject = root.normalizedSubjectText(bookSubject);
        const normalizedChipSubject = root.normalizedSubjectText(chipSubject);
        return normalizedBookSubject === normalizedChipSubject || normalizedBookSubject.startsWith(normalizedChipSubject + "/");
    }

    function subjectIsHovered(subject) {
        const subjects = root.activeSubjectList();
        for (let i = 0; i < subjects.length; ++i) {
            if (root.subjectMatches(subjects[i], subject)) {
                return true;
            }
        }
        return false;
    }

    function togglePinnedBookSubject(subject) {
        if (!root.subjectAssignmentActive || !applicationWindow().bookListModel) {
            return false;
        }

        const currentSubjects = root.subjectList(root.pinnedSubjects);
        const updatedSubjects = [];
        let subjectWasAssigned = false;

        for (let i = 0; i < currentSubjects.length; ++i) {
            if (root.subjectMatches(currentSubjects[i], subject)) {
                subjectWasAssigned = true;
            } else {
                updatedSubjects.push(currentSubjects[i]);
            }
        }

        if (!subjectWasAssigned) {
            updatedSubjects.push(subject);
        }

        root.pinnedSubjects = updatedSubjects;
        if (root.hoveredSubjectBook === root.pinnedSubjectBook) {
            root.hoveredSubjects = updatedSubjects;
        }
        applicationWindow().bookListModel.setBookData(root.pinnedSubjectBook, "subjects", updatedSubjects.join(", "));
        root.scheduleViewRefresh();
        return true;
    }

    function subjectHighlightFill(active, hoveredMatch, controlHovered) {
        const highlight = Kirigami.Theme.highlightColor;
        if (active) {
            return highlight;
        }
        if (hoveredMatch) {
            return Qt.rgba(highlight.r, highlight.g, highlight.b, 0.30);
        }
        if (controlHovered) {
            return Qt.rgba(highlight.r, highlight.g, highlight.b, 0.10);
        }
        return Kirigami.Theme.backgroundColor;
    }

    function subjectHighlightBorder(active, hoveredMatch, controlHovered) {
        const highlight = Kirigami.Theme.highlightColor;
        if (active || hoveredMatch) {
            return Qt.rgba(highlight.r, highlight.g, highlight.b, 0.9);
        }
        if (controlHovered) {
            return Qt.rgba(highlight.r, highlight.g, highlight.b, 0.55);
        }
        return Kirigami.Theme.disabledTextColor;
    }

    function subjectHighlightText(active, hoveredMatch) {
        return active ? Kirigami.Theme.highlightedTextColor : hoveredMatch ? Kirigami.Theme.highlightColor : Kirigami.Theme.textColor;
    }

    function editorCommand() {
        const command = (Config.editorCommand || "").trim();
        return command.length > 0 ? command : (Config.editorPath || "").trim();
    }

    function pdfEditorCommand() {
        return (Config.pdfEditorCommand || "").trim();
    }

    function isPdfBook(fileName) {
        return fileName && fileName.toString().toLocaleLowerCase().endsWith(".pdf");
    }

    function editorCommandForFile(fileName) {
        return root.isPdfBook(fileName) ? root.pdfEditorCommand() : root.editorCommand();
    }

    function canEditBook(fileName) {
        return root.editorCommandForFile(fileName).length > 0 && fileName && fileName.length > 0;
    }

    function editBook(fileName, entry) {
        if (!canEditBook(fileName)) {
            return;
        }

        if (root.isPdfBook(fileName)) {
            if (!ExternalProcess.startDetached(root.pdfEditorCommand(), [fileName])) {
                console.warn("Unable to start PDF editor for:", fileName);
            }
            return;
        }

        const bookId = entry ? (entry.uniqueIdentifier || entry.identifier || "") : "";
        const session = EditorSessionStore.createOrReuseEditorSession(bookId, fileName);
        if (!session || session.error) {
            console.warn("Unable to create editor session:", session && session.message ? session.message : "");
            return;
        }

        if (session.reused && ExternalProcess.isEditorSessionRunning(session.sessionId)) {
            if (!ExternalProcess.activateWindowForFile(session.workingCopyPath)) {
                console.warn("Unable to activate existing editor window for:", session.workingCopyPath);
            }
            return;
        }

        if (!ExternalProcess.startEditorSession(session.sessionId, editorCommand(), [session.workingCopyPath])) {
            if (!session.reused) {
                EditorSessionStore.finishSession(session.sessionId);
            }
            if (!ExternalProcess.activateWindowForFile(session.workingCopyPath)) {
                console.warn("Unable to start or activate editor for:", session.workingCopyPath);
            }
        }
    }

    function confirmRemoveBook(fileName) {
        if (!fileName || fileName.length === 0) {
            return;
        }

        removeBookDialog.openForBook(fileName);
    }

    function refreshBook(fileName) {
        if (!fileName || fileName.length === 0 || !applicationWindow().bookListModel) {
            return;
        }

        if (typeof applicationWindow().refreshBookWithWatermarkPolicy === "function") {
            applicationWindow().refreshBookWithWatermarkPolicy(fileName);
            return;
        }

        applicationWindow().bookListModel.refreshBookFromFile(fileName);
    }

    function activeOverviewView(grouped) {
        return grouped ? groupedBookListView : contentDirectoryView;
    }

    function clampFlickableContentY(view, contentY) {
        const originY = Number(view.originY || 0);
        const maximumY = originY + Math.max(0, Number(view.contentHeight || 0) - Number(view.height || 0));
        return Math.max(originY, Math.min(Number(contentY || 0), maximumY));
    }

    function captureOverviewPosition() {
        if (!root.componentReady || !root.refreshActive) {
            return null;
        }

        const grouped = root.bookGroupingVisible;
        const view = root.activeOverviewView(grouped);
        if (!view) {
            return null;
        }

        return {
            grouped: grouped,
            contentY: view.contentY,
            currentIndex: view.currentIndex
        };
    }

    function restoreOverviewPosition(snapshot) {
        if (!snapshot) {
            return;
        }

        const view = root.activeOverviewView(snapshot.grouped);
        if (!view || !view.visible) {
            return;
        }

        if (snapshot.currentIndex >= -1 && snapshot.currentIndex < view.count) {
            view.currentIndex = snapshot.currentIndex;
        }
        view.contentY = root.clampFlickableContentY(view, snapshot.contentY);
    }

    function scheduleOverviewPositionRestore(snapshot) {
        if (!snapshot) {
            return;
        }

        root.pendingOverviewPositionRestore = snapshot;
        root.pendingOverviewPositionRestoreAttempts = 6;
        overviewPositionRestoreTimer.restart();
    }

    function escapeHtml(text) {
        return String(text).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&#39;");
    }

    function refreshView() {
        if (!root.refreshActive) {
            root.pendingViewRefresh = true;
            sortProxy.sourceModel = null;
            root.bookGroups = [];
            root.bookGroupRows = [];
            return;
        }

        sortProxy.sourceModel = root.currentEntriesModel();
        sortProxy.invalidate();
        sortProxy.setFilterFixedString(root.searchText.trim());
        root.refreshBookGroups();
        root.scheduleBookGroupPreparation();
    }

    function scheduleViewRefresh() {
        if (!root.refreshActive) {
            root.pendingViewRefresh = true;
            return;
        }
        if (root.viewRefreshScheduled) {
            return;
        }

        root.viewRefreshScheduled = true;
        Qt.callLater(() => {
            root.viewRefreshScheduled = false;
            root.refreshView();
        });
    }

    function scheduleBookGroupPreparation(delay) {
        if (!root.refreshActive) {
            root.pendingBookGroupPreparationDelay = delay === undefined ? 9000 : delay;
            deferredBookGroupPreparationTimer.stop();
            return;
        }
        if (!root.showBookCount || root.groupingEnabled || !root.groupingAvailable || root.searchActive) {
            deferredBookGroupPreparationTimer.stop();
            return;
        }

        deferredBookGroupPreparationTimer.interval = delay === undefined ? 9000 : delay;
        deferredBookGroupPreparationTimer.restart();
    }

    function triggerAddBooks() {
        if (root.addBookAction) {
            root.addBookAction.trigger();
            return;
        }

        if (applicationWindow() && typeof applicationWindow().openAddBooksDialog === "function") {
            applicationWindow().openAddBooksDialog();
        }
    }

    function scheduleSubjectPopulation() {
        if (!root.refreshActive) {
            root.pendingSubjectRefresh = true;
            deferredSubjectPopulationTimer.stop();
            return;
        }

        const model = applicationWindow().bookListModel;
        if (!model || model.subjectCategoryModelPopulated) {
            return;
        }

        deferredSubjectPopulationTimer.restart();
    }

    function scheduleSubjectRefresh() {
        if (!root.refreshActive) {
            root.pendingSubjectRefresh = true;
            root.pendingViewRefresh = true;
            return;
        }
        if (root.subjectRefreshScheduled) {
            return;
        }

        root.subjectRefreshScheduled = true;
        Qt.callLater(() => {
            root.subjectRefreshScheduled = false;
            if (!root.refreshActive) {
                root.pendingSubjectRefresh = true;
                root.pendingViewRefresh = true;
                return;
            }
            root.refreshSubjectChips();
            root.scheduleViewRefresh();
        });
    }

    function handleRefreshActiveChanged() {
        if (!root.refreshActive) {
            sortProxy.sourceModel = null;
            root.bookGroups = [];
            root.bookGroupRows = [];
            deferredBookGroupPreparationTimer.stop();
            deferredSubjectPopulationTimer.stop();
            return;
        }

        if (root.pendingSubjectRefresh) {
            root.pendingSubjectRefresh = false;
            root.refreshSubjectChips();
        }
        if (root.pendingViewRefresh) {
            root.pendingViewRefresh = false;
            root.scheduleViewRefresh();
        } else {
            root.scheduleViewRefresh();
        }
        if (root.pendingBookGroupPreparationDelay >= 0) {
            const delay = root.pendingBookGroupPreparationDelay;
            root.pendingBookGroupPreparationDelay = -1;
            root.scheduleBookGroupPreparation(delay);
        }
        root.scheduleSubjectPopulation();
    }

    function populateSubjectModelNow() {
        const model = applicationWindow().bookListModel;
        if (!model || model.subjectCategoryModelPopulated) {
            return;
        }

        deferredSubjectPopulationTimer.stop();
        model.populateSubjectCategoryModel();
    }

    Timer {
        id: deferredSubjectPopulationTimer

        interval: 5000
        repeat: false
        onTriggered: {
            const model = applicationWindow().bookListModel;
            if (!model || model.subjectCategoryModelPopulated) {
                return;
            }
            if (!model.cacheLoaded) {
                restart();
                return;
            }

            model.populateSubjectCategoryModel();
        }
    }

    Timer {
        id: deferredBookGroupPreparationTimer

        interval: 9000
        repeat: false
        onTriggered: root.prepareCurrentBookGroups()
    }

    Timer {
        id: suppressNextBookClickTimer

        interval: 0
        repeat: false
        onTriggered: root.suppressNextBookClick = false
    }

    Timer {
        id: overviewPositionRestoreTimer

        interval: 16
        repeat: false
        onTriggered: {
            root.restoreOverviewPosition(root.pendingOverviewPositionRestore);
            root.pendingOverviewPositionRestoreAttempts -= 1;
            if (root.pendingOverviewPositionRestoreAttempts > 0) {
                restart();
                return;
            }

            root.pendingOverviewPositionRestore = null;
        }
    }

    Connections {
        target: ExternalProcess
        function onEditorSessionProcessStarted(sessionId, processId) {
            EditorSessionStore.noteEditorProcessStarted(sessionId, processId);
        }
        function onEditorSessionFileChanged(sessionId, file) {
            EditorSessionStore.scheduleImport(sessionId);
        }
        function onEditorSessionFinished(sessionId, exitCode, exitStatus) {
            EditorSessionStore.finishSession(sessionId);
        }
    }

    Connections {
        target: EditorSessionStore
        function onEditorSessionImportFinished(sessionId, status, message, workingCopyPath, newStateId, anchorValidationIssues) {
            if (status === "imported") {
                console.log("Editor working copy imported as state:", newStateId);
                root.scheduleViewRefresh();
                return;
            }
            if (status !== "unchanged") {
                console.warn("Editor import failed:", status, message, "Invalid anchors:", anchorValidationIssues ? anchorValidationIssues.length : 0, "Recovery copy:", workingCopyPath);
            }
        }
    }

    Kirigami.Action {
        id: addBookActionProxy

        text: root.addBooksActionText
        tooltip: root.addBooksActionText
        displayHint: Kirigami.DisplayHint.KeepVisible
        icon.name: root.addBooksActionIconName
        enabled: root.addBooksActionEnabled
        onTriggered: root.triggerAddBooks()
    }

    actions: [
        Kirigami.Action {
            text: i18nc("@action:button Toggle grouped book view", "Group")
            tooltip: text
            icon.name: root.groupingEnabled ? "view-list-details" : "view-grid"
            checkable: true
            checked: root.groupingEnabled
            enabled: root.groupingAvailable
            onTriggered: {
                root.groupingEnabled = !root.groupingEnabled;
            }
        },
        Kirigami.Action {
            text: i18nc("@action:button", "Sort")
            tooltip: text
            icon.name: "view-sort"
            Kirigami.Action {
                text: i18nc("@action:inmenu Sort books by title", "Title")
                icon.name: "view-sort-ascending-name"
                checkable: true
                checked: sortProxy.sortRoleName === "titleSort"
                onTriggered: {
                    sortProxy.sortRoleName = "titleSort";
                    sortProxy.sortOrder = Qt.AscendingOrder;
                    root.scheduleViewRefresh();
                }
            }
            Kirigami.Action {
                text: i18nc("@action:inmenu Sort books by main topic", "Main Topic")
                icon.name: "tag-symbolic"
                checkable: true
                checked: sortProxy.sortRoleName === "mainSubjectSort"
                onTriggered: {
                    sortProxy.sortRoleName = "mainSubjectSort";
                    sortProxy.sortOrder = Qt.AscendingOrder;
                    root.scheduleViewRefresh();
                }
            }
            Kirigami.Action {
                text: i18nc("@action:inmenu Sort books by author", "Author")
                icon.name: "actor"
                checkable: true
                checked: sortProxy.sortRoleName === "authorSort"
                onTriggered: {
                    sortProxy.sortRoleName = "authorSort";
                    sortProxy.sortOrder = Qt.AscendingOrder;
                    root.scheduleViewRefresh();
                }
            }
            Kirigami.Action {
                text: i18nc("@action:inmenu Sort books by file type", "Type")
                icon.name: "documentinfo"
                checkable: true
                checked: sortProxy.sortRoleName === "typeSort"
                onTriggered: {
                    sortProxy.sortRoleName = "typeSort";
                    sortProxy.sortOrder = Qt.AscendingOrder;
                    root.scheduleViewRefresh();
                }
            }
            Kirigami.Action {
                text: i18nc("@action:inmenu Sort books by last access", "Last Access")
                icon.name: "clock"
                checkable: true
                checked: sortProxy.sortRoleName === "lastOpenedTime"
                onTriggered: {
                    sortProxy.sortRoleName = "lastOpenedTime";
                    sortProxy.sortOrder = Qt.DescendingOrder;
                    root.scheduleViewRefresh();
                }
            }
        }
    ]

    KItemModels.KSortFilterProxyModel {
        id: sortProxy
        sourceModel: null
        sortRoleName: "lastOpenedTime"
        sortOrder: Qt.DescendingOrder
        filterRole: CategoryEntriesModel.TitleRole
        filterCaseSensitivity: Qt.CaseInsensitive
        dynamicSortFilter: true

        Component.onCompleted: setFilterFixedString(root.searchText.trim())
    }

    Connections {
        target: root.subjectModel

        function onCountChanged() {
            root.scheduleSubjectRefresh();
        }

        function onRowsInserted() {
            root.scheduleSubjectRefresh();
        }

        function onRowsRemoved() {
            root.scheduleSubjectRefresh();
        }

        function onModelReset() {
            root.clearSubjectFilter();
            root.clearHoveredSubjects(root.hoveredSubjectBook);
            root.scheduleSubjectRefresh();
        }
    }

    Connections {
        target: root.bookListModel

        function onCountChanged() {
            root.scheduleSubjectPopulation();
            root.scheduleViewRefresh();
        }

        function onDataChanged() {
            root.scheduleViewRefresh();
        }

        function onCacheLoadedChanged() {
            root.scheduleSubjectPopulation();
            root.scheduleBookGroupPreparation(9000);
        }

        function onSubjectCategoryModelChanged() {
            root.scheduleSubjectRefresh();
        }

        function onSubjectCategoryModelPopulatedChanged() {
            root.scheduleSubjectRefresh();
        }

        function onRowsInserted() {
            root.scheduleViewRefresh();
        }

        function onRowsRemoved() {
            root.scheduleViewRefresh();
        }

        function onModelReset() {
            root.scheduleViewRefresh();
        }

        function onEntryDataUpdated(entry) {
            if (entry && entry.filename === root.pinnedSubjectBook) {
                root.pinnedSubjects = root.subjectList(entry.subjects || entry.genres);
            }
            root.scheduleSubjectRefresh();
        }

        function onEntryRemoved(entry) {
            if (entry && entry.filename === root.pinnedSubjectBook) {
                root.clearPinnedSubjectBook();
            }
            root.scheduleSubjectRefresh();
        }
    }

    Connections {
        target: applicationWindow().bookListModel

        function onBookRefreshAboutToUpdate(fileName) {
            root.scheduleOverviewPositionRestore(root.captureOverviewPosition());
        }
    }

    Component.onCompleted: {
        root.componentReady = true;
        root.refreshSubjectChips();
        root.handleRefreshActiveChanged();
        root.scheduleViewRefresh();
        root.scheduleSubjectPopulation();
        root.scheduleBookGroupPreparation();
    }

    QQC2.Dialog {
        id: removeBookDialog

        property string bookFileName: ""
        property string location: ""

        parent: QQC2.Overlay.overlay ? QQC2.Overlay.overlay : root
        modal: true
        title: i18nc("@title:window", "Remove Book")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel

        function openForBook(fileName) {
            bookFileName = fileName;
            location = fileName;
            deleteFileCheckBox.checked = false;
            open();
        }

        onAccepted: {
            applicationWindow().bookListModel.removeBook(bookFileName, deleteFileCheckBox.checked);
            bookFileName = "";
            location = "";
        }

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.largeSpacing

            QQC2.Label {
                Layout.fillWidth: true
                text: i18nc("@info", "Do you really want to remove the book from the library? All saved data such as progress and annotations will be lost.")
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                QQC2.CheckBox {
                    id: deleteFileCheckBox

                    Layout.alignment: Qt.AlignTop
                    checked: false
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18nc("@option:check %1 is the book file location", "Also delete the book from the location:<br><b>%1</b><br>Delete it.", root.escapeHtml(removeBookDialog.location))
                    textFormat: Text.RichText
                    wrapMode: Text.WrapAnywhere

                    TapHandler {
                        onTapped: deleteFileCheckBox.checked = !deleteFileCheckBox.checked
                    }
                }
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        QQC2.Pane {
            id: subjectFilterPane

            Layout.fillWidth: true
            visible: root.subjectFilterVisible
            implicitHeight: visible ? subjectFilterContent.implicitHeight + topPadding + bottomPadding : 0
            leftPadding: Kirigami.Units.smallSpacing
            rightPadding: Kirigami.Units.smallSpacing
            topPadding: Kirigami.Units.smallSpacing
            bottomPadding: Kirigami.Units.smallSpacing

            contentItem: ColumnLayout {
                id: subjectFilterContent

                width: subjectFilterPane.availableWidth
                spacing: Kirigami.Units.smallSpacing

                Repeater {
                    model: root.subjectChipGroups

                    delegate: ColumnLayout {
                        id: subjectLevelGroup

                        readonly property var group: modelData || ({})
                        readonly property var chips: group.chips || []

                        Layout.fillWidth: true
                        Layout.topMargin: subjectLevelGroup.group.level > 1 ? Kirigami.Units.smallSpacing : 0
                        spacing: Kirigami.Units.smallSpacing

                        QQC2.Label {
                            Layout.fillWidth: true
                            text: subjectLevelGroup.group.title || ""
                            color: Kirigami.Theme.disabledTextColor
                            horizontalAlignment: Text.AlignHCenter
                            font.weight: Font.DemiBold
                            font.pixelSize: Math.round(Kirigami.Units.gridUnit * 0.78)
                            elide: Text.ElideRight
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 1
                            color: Kirigami.Theme.disabledTextColor
                            opacity: 0.45
                        }

                        Flow {
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing

                            Repeater {
                                model: subjectLevelGroup.chips

                                delegate: QQC2.Button {
                                    id: subjectButton

                                    readonly property var chip: modelData || ({})
                                    readonly property string title: chip.title || ""
                                    readonly property string localizedTitle: chip.localizedTitle || title
                                    readonly property string localizedPathTitle: chip.localizedPathTitle || localizedTitle
                                    readonly property int categoryEntriesCount: chip.categoryEntriesCount || 0
                                    readonly property int subjectLevel: chip.subjectLevel || 1
                                    readonly property var categoryEntriesModel: chip.categoryEntriesModel || null
                                    readonly property bool hoveredSubjectMatch: root.subjectIsHovered(subjectButton.title)

                                    visible: categoryEntriesCount > 0
                                    text: i18nc("@action:filter %1 is the topic, %2 is the number of books", "%1 (%2)", localizedTitle, categoryEntriesCount)
                                    checkable: true
                                    checked: root.selectedSubject === subjectButton.title
                                    highlighted: subjectButton.checked || subjectButton.hoveredSubjectMatch
                                    leftPadding: Kirigami.Units.largeSpacing
                                    rightPadding: Kirigami.Units.largeSpacing
                                    topPadding: Kirigami.Units.smallSpacing
                                    bottomPadding: Kirigami.Units.smallSpacing
                                    QQC2.ToolTip.text: localizedPathTitle
                                    QQC2.ToolTip.visible: hovered && localizedPathTitle !== localizedTitle
                                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                                    contentItem: QQC2.Label {
                                        text: subjectButton.text
                                        font.family: subjectButton.font.family
                                        font.pixelSize: subjectButton.font.pixelSize
                                        font.weight: subjectButton.subjectLevel === 1 ? Font.DemiBold : Font.Normal
                                        color: root.subjectHighlightText(subjectButton.checked, subjectButton.hoveredSubjectMatch)
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                        elide: Text.ElideRight
                                    }
                                    background: Rectangle {
                                        implicitWidth: Kirigami.Units.gridUnit * 5
                                        implicitHeight: Kirigami.Units.gridUnit * 1.8
                                        radius: Kirigami.Units.smallSpacing
                                        color: root.subjectHighlightFill(subjectButton.checked, subjectButton.hoveredSubjectMatch, subjectButton.hovered)
                                        border.color: root.subjectHighlightBorder(subjectButton.checked, subjectButton.hoveredSubjectMatch, subjectButton.hovered)
                                        border.width: subjectButton.checked || subjectButton.hoveredSubjectMatch || subjectButton.hovered ? 2 : 1
                                    }
                                    onClicked: {
                                        if (root.togglePinnedBookSubject(subjectButton.title)) {
                                            return;
                                        }
                                        if (root.selectedSubject === subjectButton.title) {
                                            root.selectSubjectFilter("", null);
                                            return;
                                        }
                                        root.selectSubjectFilter(subjectButton.title, subjectButton.categoryEntriesModel);
                                    }
                                }
                            }
                        }
                    }
                }

                QQC2.Label {
                    visible: !root.subjectFeaturesReady && root.subjectChipGroups.length === 0
                    text: i18nc("@info placeholder", "Loading topics…")
                    opacity: 0.7
                    leftPadding: Kirigami.Units.largeSpacing
                    rightPadding: Kirigami.Units.largeSpacing
                    topPadding: Kirigami.Units.smallSpacing
                    bottomPadding: Kirigami.Units.smallSpacing
                }
            }
        }

        GridView {
            id: contentDirectoryView

            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.bookGroupingVisible
            leftMargin: Kirigami.Units.smallSpacing
            rightMargin: Kirigami.Units.smallSpacing
            topMargin: Kirigami.Units.smallSpacing
            bottomMargin: Kirigami.Units.smallSpacing

            model: sortProxy

            cellWidth: {
                return root.bookGridCellWidth(contentDirectoryView.width);
            }
            cellHeight: {
                return root.bookGridCellHeight(cellWidth);
            }
            currentIndex: -1
            reuseItems: true
            activeFocusOnTab: true
            keyNavigationEnabled: true
            onVisibleChanged: if (visible && !root.librarySearchFieldActiveFocus) {
                forceActiveFocus();
            }

            Keys.onPressed: event => {
                if (root.handleAuthorJumpKey(event)) {
                    event.accepted = true;
                }
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                onWheel: event => root.zoomLibraryFromWheel(event)
            }

            delegate: GridBrowserDelegate {
                id: bookDelegate

                required property string thumbnail
                required property string title
                required property string localizedTitle
                required property string filename
                required property string mainSubject
                required property var author
                required property var entry
                required property var locations
                required property var currentLocation
                required property var subjects
                required property bool passwordProtected
                required property int categoryEntriesCount
                required property var categoryEntriesModel

                width: root.bookTileWidth(contentDirectoryView.width)
                height: contentDirectoryView.cellHeight
                hoverEnabled: true
                highlighted: root.pinnedSubjectBook === bookDelegate.filename

                imageUrl: categoryEntriesModel === '' && thumbnail !== '' ? ('file://' + thumbnail) : ''
                iconName: if (categoryEntriesModel !== '') {
                    return thumbnail;
                } else if (thumbnail === '') {
                    return 'application-epub+zip';
                } else {
                    return '';
                }

                mainText: root.bookTileMainText(bookDelegate.localizedTitle, bookDelegate.categoryEntriesCount, bookDelegate.categoryEntriesModel)
                secondaryText: root.bookSecondaryText(bookDelegate.mainSubject, bookDelegate.author, bookDelegate.categoryEntriesModel === "")
                typeIconSource: root.typeIconSourceForFile(bookDelegate.filename, bookDelegate.categoryEntriesModel)
                showPasswordBadge: bookDelegate.passwordProtected

                onHoveredChanged: {
                    if (bookDelegate.categoryEntriesModel !== "") {
                        return;
                    }
                    if (bookDelegate.hovered) {
                        root.setHoveredSubjects(bookDelegate.filename, bookDelegate.subjects);
                    } else {
                        root.clearHoveredSubjects(bookDelegate.filename);
                    }
                }

                Component.onDestruction: root.clearHoveredSubjects(bookDelegate.filename)

                onClicked: {
                    if (root.suppressNextBookClick) {
                        root.suppressNextBookClick = false;
                        return;
                    }
                    if (categoryEntriesModel) {
                        Navigation.openLibrary(localizedTitle, categoryEntriesModel, false);
                    } else {
                        Navigation.openBook(filename, locations, currentLocation, entry, false);
                    }
                }

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: {
                        menu.entry = bookDelegate.entry;
                        menu.filename = bookDelegate.filename;
                        menu.isBook = bookDelegate.categoryEntriesModel === "";
                        menu.popup();
                    }
                }

                TapHandler {
                    acceptedButtons: Qt.MiddleButton
                    enabled: bookDelegate.categoryEntriesModel === ""
                    onTapped: {
                        root.suppressNextBookClick = true;
                        root.togglePinnedSubjectBook(bookDelegate.filename, bookDelegate.localizedTitle, bookDelegate.subjects);
                        suppressNextBookClickTimer.restart();
                    }
                }
            }

            Components.ConvergentContextMenu {
                id: menu

                property var entry: null
                property string filename: ""
                property bool isBook: false

                QQC2.Action {
                    icon.name: 'view-refresh'
                    text: i18nc("@action:inmenu", "Refresh Book Metadata")
                    enabled: menu.isBook
                    onTriggered: root.refreshBook(menu.filename)
                }

                QQC2.Action {
                    icon.name: 'documentinfo-symbolic'
                    text: i18nc("@action:inmenu", "Book Details")
                    onTriggered: applicationWindow().pageStack.pushDialogLayer(Qt.resolvedUrl("./BookDetailsPage.qml"), {
                        metadata: menu.entry,
                        bookListModel: applicationWindow().bookListModel
                    })
                }

                QQC2.Action {
                    icon.name: 'document-edit'
                    text: i18nc("@action:inmenu", "Edit Book")
                    enabled: menu.isBook && root.canEditBook(menu.filename)
                    onTriggered: root.editBook(menu.filename, menu.entry)
                }

                QQC2.Action {
                    icon.name: 'edit-delete-remove'
                    text: i18nc("@action:inmenu", "Remove from Library")
                    enabled: menu.isBook
                    onTriggered: root.confirmRemoveBook(menu.filename)
                }
            }

            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                width: parent.width - (Kirigami.Units.largeSpacing * 4)
                visible: contentDirectoryView.count === 0
                icon.name: "application-epub+zip"
                text: root.searchActive ? i18nc("@info placeholder", "No books found") : i18nc("@info placeholder", "Add some books")
                helpfulAction: root.searchActive ? null : addBookActionProxy
            }
        }

        Item {
            id: mainSubjectGroupContainer

            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.bookGroupingVisible
            activeFocusOnTab: true
            focus: visible
            onVisibleChanged: if (visible && !root.librarySearchFieldActiveFocus) {
                forceActiveFocus();
            }

            Keys.onPressed: event => {
                if (root.handleAuthorJumpKey(event)) {
                    event.accepted = true;
                }
            }

            ListView {
                id: groupedBookListView
                anchors.fill: parent
                clip: true
                model: root.bookGroupRows
                reuseItems: true
                cacheBuffer: height * 1.5
                spacing: Kirigami.Units.smallSpacing
                currentIndex: -1
                activeFocusOnTab: true
                boundsBehavior: Flickable.StopAtBounds
                onWidthChanged: root.scheduleBookGroupRowsRefresh()
                Keys.onPressed: event => {
                    if (root.handleAuthorJumpKey(event)) {
                        event.accepted = true;
                    }
                }

                delegate: Item {
                    id: groupedRowDelegate

                    required property var modelData

                    readonly property var row: modelData || ({})
                    readonly property bool headerRow: row.type === "header"
                    readonly property bool booksRow: row.type === "books"
                    readonly property int tileWidth: root.bookTileWidth(width)
                    readonly property int tileHeight: root.bookTileHeight(width)

                    width: ListView.view.width
                    height: {
                        if (headerRow) {
                            return groupedHeaderContent.implicitHeight;
                        } else if (booksRow) {
                            return tileHeight;
                        }
                        return Kirigami.Units.largeSpacing;
                    }

                    ColumnLayout {
                        id: groupedHeaderContent

                        anchors.left: parent.left
                        anchors.right: parent.right
                        visible: groupedRowDelegate.headerRow
                        spacing: Kirigami.Units.smallSpacing

                        QQC2.Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: Kirigami.Units.largeSpacing
                            Layout.rightMargin: Kirigami.Units.largeSpacing
                            Layout.topMargin: Kirigami.Units.smallSpacing
                            text: groupedRowDelegate.row.title || ""
                            color: Kirigami.Theme.disabledTextColor
                            horizontalAlignment: Text.AlignHCenter
                            font.weight: Font.DemiBold
                            font.pixelSize: Math.round(Kirigami.Units.gridUnit * 0.95)
                            elide: Text.ElideRight
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.leftMargin: Kirigami.Units.largeSpacing
                            Layout.rightMargin: Kirigami.Units.largeSpacing
                            implicitHeight: 1
                            color: Kirigami.Theme.disabledTextColor
                            opacity: 0.45
                        }
                    }

                    Row {
                        id: groupedBookRow

                        x: Kirigami.Units.smallSpacing
                        width: Math.max(1, parent.width - Kirigami.Units.smallSpacing * 2)
                        height: groupedRowDelegate.tileHeight
                        visible: groupedRowDelegate.booksRow
                        spacing: Kirigami.Units.smallSpacing

                        Repeater {
                            model: groupedRowDelegate.row.books || []

                            delegate: GridBrowserDelegate {
                                id: groupedBookDelegate

                                required property var modelData

                                readonly property var book: groupedBookDelegate.modelData || ({})
                                readonly property string thumbnail: book.thumbnail || ""
                                readonly property string localizedTitle: book.localizedTitle || book.title || ""
                                readonly property string filename: book.filename || ""
                                readonly property string mainSubject: book.mainSubject || ""
                                readonly property var author: book.author || []
                                readonly property var entry: book.entry || ({})
                                readonly property var locations: book.locations || ""
                                readonly property var currentLocation: book.currentLocation || ""
                                readonly property var subjects: book.subjects || []
                                readonly property bool passwordProtected: Boolean(book.passwordProtected || false)
                                readonly property var categoryEntriesModel: book.categoryEntriesModel || ""

                                width: groupedRowDelegate.tileWidth
                                height: groupedRowDelegate.tileHeight
                                hoverEnabled: true
                                highlighted: root.pinnedSubjectBook === groupedBookDelegate.filename

                                imageUrl: categoryEntriesModel === "" && thumbnail !== "" ? ("file://" + thumbnail) : ""
                                iconName: if (categoryEntriesModel !== "") {
                                    return thumbnail;
                                } else if (thumbnail === "") {
                                    return "application-epub+zip";
                                } else {
                                    return "";
                                }
                                mainText: localizedTitle
                                secondaryText: root.bookSecondaryText(mainSubject, author, true)
                                currentProgress: Number(book.currentProgress || 0)
                                typeIconSource: root.typeIconSourceForFile(filename, categoryEntriesModel)
                                showPasswordBadge: groupedBookDelegate.passwordProtected

                                onHoveredChanged: {
                                    if (hovered) {
                                        root.setHoveredSubjects(filename, subjects);
                                    } else {
                                        root.clearHoveredSubjects(filename);
                                    }
                                }

                                Component.onDestruction: root.clearHoveredSubjects(filename)

                                onClicked: {
                                    if (root.suppressNextBookClick) {
                                        root.suppressNextBookClick = false;
                                        return;
                                    }
                                    Navigation.openBook(filename, locations, currentLocation, entry, false);
                                }

                                TapHandler {
                                    acceptedButtons: Qt.RightButton
                                    onTapped: {
                                        groupedMenu.entry = groupedBookDelegate.entry;
                                        groupedMenu.filename = groupedBookDelegate.filename;
                                        groupedMenu.isBook = true;
                                        groupedMenu.popup();
                                    }
                                }

                                TapHandler {
                                    acceptedButtons: Qt.MiddleButton
                                    onTapped: {
                                        root.suppressNextBookClick = true;
                                        root.togglePinnedSubjectBook(groupedBookDelegate.filename, groupedBookDelegate.localizedTitle, groupedBookDelegate.subjects);
                                        suppressNextBookClickTimer.restart();
                                    }
                                }
                            }
                        }
                    }
                }
            }

            MouseArea {
                anchors.fill: groupedBookListView
                acceptedButtons: Qt.NoButton
                onWheel: event => root.zoomLibraryFromWheel(event)
            }

            Components.ConvergentContextMenu {
                id: groupedMenu

                property var entry: null
                property string filename: ""
                property bool isBook: true

                QQC2.Action {
                    icon.name: 'view-refresh'
                    text: i18nc("@action:inmenu", "Refresh Book Metadata")
                    enabled: groupedMenu.isBook
                    onTriggered: root.refreshBook(groupedMenu.filename)
                }

                QQC2.Action {
                    icon.name: 'documentinfo-symbolic'
                    text: i18nc("@action:inmenu", "Book Details")
                    onTriggered: applicationWindow().pageStack.pushDialogLayer(Qt.resolvedUrl("./BookDetailsPage.qml"), {
                        metadata: groupedMenu.entry,
                        bookListModel: applicationWindow().bookListModel
                    })
                }

                QQC2.Action {
                    icon.name: 'document-edit'
                    text: i18nc("@action:inmenu", "Edit Book")
                    enabled: groupedMenu.isBook && root.canEditBook(groupedMenu.filename)
                    onTriggered: root.editBook(groupedMenu.filename, groupedMenu.entry)
                }

                QQC2.Action {
                    icon.name: 'edit-delete-remove'
                    text: i18nc("@action:inmenu", "Remove from Library")
                    enabled: groupedMenu.isBook
                    onTriggered: root.confirmRemoveBook(groupedMenu.filename)
                }
            }

            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                width: parent.width - (Kirigami.Units.largeSpacing * 4)
                visible: root.bookGroups.length === 0
                icon.name: "application-epub+zip"
                text: root.searchActive ? i18nc("@info placeholder", "No books found") : i18nc("@info placeholder", "Add some books")
                helpfulAction: root.searchActive ? null : addBookActionProxy
            }
        }
    }
}
