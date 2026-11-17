// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Window
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import Qt.labs.platform
import org.kde.arianna
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.delegates as Delegates
import org.kde.config as KConfig

Kirigami.ApplicationWindow {
    id: root

    property bool isLoading: true
    property string librarySearchText: ""
    property bool readerFullScreen: false
    property bool balooImportRunning: false
    property bool balooImportSearched: false
    property bool calibreImportRunning: false
    property string bookImportStatus: ""
    property bool importRejectionsPending: false
    property var pendingWatermarkRemovalQueue: []
    property var activeWatermarkRemovalRequest: null
    readonly property bool directStartupReader: typeof startupDirectReaderMode !== "undefined" && startupDirectReaderMode === true
    readonly property string ebookFileNameFilter: i18nc("@item:inlistbox file type filter", "eBook files (*.epub *.pdf *.cb* *.fb2 *.fb2zip)")

    title: i18n("Arianna")

    LibraryMaintenance {
        id: libraryMaintenance
    }

    ReaderHistory {
        id: readerHistory
    }

    Kirigami.PagePool {
        id: maintenancePages
    }

    Kirigami.PagePool {
        id: importPages
    }

    signal workStructureSaved(string bookId)
    property var workStructureQueue: []
    function showWorkStructure(bookId) {
        if (!bookId) return;
        if (workStructureDialog.visible) {
            if (!workStructureQueue.includes(bookId)) workStructureQueue = workStructureQueue.concat([bookId]);
        } else workStructureDialog.showForBook(bookId);
    }
    WorkStructureDialog {
        id: workStructureDialog
        parent: QQC2.Overlay.overlay
        onStructureSaved: bookId => root.workStructureSaved(bookId)
        onClosed: {
            if (root.workStructureQueue.length) {
                const next = root.workStructureQueue[0];
                root.workStructureQueue = root.workStructureQueue.slice(1);
                Qt.callLater(() => root.showWorkStructure(next));
            }
        }
    }

    function showImportPage(source = "") {
        const page = importPages.loadPageWithProperties(Qt.resolvedUrl("./ImportPageView.qml"), {
            importController: root,
            balooModel: balooImportModel
        });
        if (!page) {
            return;
        }
        root.readerFullScreen = false;
        goHomeButton.checked = false;
        if (root.pageStack.currentItem !== page) {
            root.pageStack.replace(page);
        }
        if (source.length > 0) {
            page.showSource(source);
        }
        if (navigationDrawer.modal) {
            navigationDrawer.close();
        }
        root.updateOverviewWindowTitle();
    }

    function showMaintenancePage(category = "") {
        const page = maintenancePages.loadPageWithProperties(Qt.resolvedUrl("./MaintaincePage.qml"), {
            checker: libraryMaintenance
        });
        if (!page) {
            return;
        }
        root.readerFullScreen = false;
        goHomeButton.checked = false;
        if (root.pageStack.currentItem !== page) {
            root.pageStack.replace(page);
        }
        if (category.length > 0) {
            page.showCheck(category);
        } else if (libraryMaintenance.category.length === 0) {
            page.showCheck("database");
        }
        if (navigationDrawer.modal) {
            navigationDrawer.close();
        }
        root.updateOverviewWindowTitle();
    }

    QQC2.Dialog {
        id: editorCacheNotice
        objectName: "editorCacheNotice"
        parent: QQC2.Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: i18n("Editor Start Blocked")
        standardButtons: QQC2.Dialog.Close
        width: Math.min(Kirigami.Units.gridUnit * 40, parent ? parent.width - Kirigami.Units.gridUnit * 2 : 720)
        property string message: ""
        contentItem: ColumnLayout {
            QQC2.Label {
                Layout.fillWidth: true
                text: editorCacheNotice.message
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            QQC2.Button {
                objectName: "reviewEditorCacheCopies"
                text: i18nc("@action:button", "Cache and Safety Backups")
                enabled: !libraryMaintenance.running
                onClicked: {
                    editorCacheNotice.close();
                    root.showMaintenancePage("cache");
                }
            }
        }
    }
    Connections {
        target: root.pageStack && root.pageStack.layers.depth === 1 && root.pageStack.currentItem
            && root.pageStack.currentItem.maintenancePage === true ? ExternalProcess : null
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
        function onEditorStartBlocked(message, workingCopies) {
            editorCacheNotice.message = message + (workingCopies.length ? "\n\n" + workingCopies.slice(0, 4).join("\n") : "");
            editorCacheNotice.open();
        }
    }

    Component.onCompleted: root.updateOverviewWindowTitle()

    function overviewWindowTitle() {
        return i18nc("@title:window, %1 is the number of books", "Library (%1) - Arianna", root.bookListModel ? root.bookListModel.count : 0);
    }

    function isOverviewLibraryPage(page) {
        return page && page.libraryPage === true && page.bookListModel === root.bookListModel;
    }

    function updateOverviewWindowTitle() {
        if (!root.pageStack || root.pageStack.layers.depth > 1) {
            return;
        }

        const page = root.pageStack.currentItem;
        root.title = page && (page.librarySearchPage === true || page.maintenancePage === true || page.importPage === true)
            ? i18nc("@title:window", "%1 - Arianna", page.title)
            : root.isOverviewLibraryPage(page) ? root.overviewWindowTitle() : i18n("Arianna");
    }

    function centerToolbarActionsFor(page) {
        if (!page) {
            return false;
        }

        try {
            return page.centerToolbarActions === true;
        } catch (error) {
            return false;
        }
    }

    function toggleReaderFullScreen() {
        root.readerFullScreen = !root.readerFullScreen;
    }

    onReaderFullScreenChanged: {
        visibility = readerFullScreen ? Window.FullScreen : Window.Windowed;
    }

    function localPathFromUrl(url) {
        const value = String(url);
        if (value.startsWith("file://")) {
            return decodeURIComponent(value.replace("file://", ""));
        }

        return value;
    }

    function fileTitleFromPath(fileName) {
        const fileTitle = String(fileName).split("/").pop();
        const extensionIndex = fileTitle.lastIndexOf(".");
        return extensionIndex > 0 ? fileTitle.substring(0, extensionIndex) : fileTitle;
    }

    function fileUrlFromLocalPath(fileName) {
        const path = String(fileName || "");
        if (path.startsWith("file://")) {
            return path;
        }

        const normalizedPath = path.replace(/\\/g, "/");
        const prefix = normalizedPath.startsWith("/") ? "file://" : "file:///";
        return prefix + encodeURI(normalizedPath).replace(/#/g, "%23").replace(/\?/g, "%3F");
    }

    function isPdfFile(fileName) {
        return localPathFromUrl(fileName).toLowerCase().endsWith(".pdf");
    }

    function pdfWatermarkFilterMode() {
        const mode = Config.pdfWatermarkFilterMode || "";
        if (mode === "permanent" || mode === "temporary") {
            return mode;
        }
        if (Config.pdfWatermarkFilterEnabled) {
            return "temporary";
        }
        return "never";
    }

    function pdfWatermarkPattern() {
        return (Config.pdfWatermarkFilterPattern || Config.defaultPdfWatermarkFilterPatternValue).trim();
    }

    function permanentPdfWatermarkFilterEnabled(fileName) {
        return root.pdfWatermarkFilterMode() === "permanent" && root.isPdfFile(fileName);
    }

    function openNextWatermarkRemovalRequest() {
        if (root.activeWatermarkRemovalRequest || permanentWatermarkRemovalDialog.opened || root.pendingWatermarkRemovalQueue.length === 0) {
            return;
        }

        const queue = root.pendingWatermarkRemovalQueue;
        root.activeWatermarkRemovalRequest = queue.shift();
        root.pendingWatermarkRemovalQueue = queue;
        permanentWatermarkRemovalDialog.open();
    }

    function finishActiveWatermarkRemoval(result) {
        const request = root.activeWatermarkRemovalRequest;
        root.activeWatermarkRemovalRequest = null;
        if (request && typeof request.callback === "function") {
            request.callback(result || {});
        }
        Qt.callLater(root.openNextWatermarkRemovalRequest);
    }

    function requestPermanentWatermarkRemoval(fileName, reason, callback) {
        const localFileName = root.localPathFromUrl(fileName);
        if (!root.permanentPdfWatermarkFilterEnabled(localFileName) || !root.bookListModel) {
            if (typeof callback === "function") {
                callback({});
            }
            return;
        }

        const result = root.bookListModel.createWatermarkFreePdf(localFileName, root.pdfWatermarkPattern());
        if (!result || !result.filtered || !result.replacementFileName) {
            if (result && result.message && !result.success) {
                console.warn("PDF watermark filter could not prepare replacement:", result.message);
            }
            if (typeof callback === "function") {
                callback(result || {});
            }
            return;
        }

        const queue = root.pendingWatermarkRemovalQueue;
        queue.push({
            fileName: localFileName,
            reason: reason || "",
            replacementFileName: result.replacementFileName,
            occurrences: result.occurrences || 0,
            message: result.message || "",
            callback: callback
        });
        root.pendingWatermarkRemovalQueue = queue;
        root.openNextWatermarkRemovalRequest();
    }

    function refreshBookWithWatermarkPolicy(fileName, refreshCover, callback) {
        if (typeof refreshCover === "function") {
            callback = refreshCover;
            refreshCover = true;
        }
        if (refreshCover === undefined) {
            refreshCover = true;
        }

        const localFileName = root.localPathFromUrl(fileName);
        const refreshMetadata = watermarkResult => {
            const refreshedEntry = root.bookListModel.refreshBookFromFile(localFileName, refreshCover);
            let message = refreshedEntry && refreshedEntry.filename ? i18n("PDF metadata refreshed.") : i18n("PDF metadata could not be refreshed.");
            if (watermarkResult && watermarkResult.message) {
                message = refreshedEntry && refreshedEntry.filename ? i18n("%1\n%2", message, watermarkResult.message) : watermarkResult.message;
            }
            if (typeof callback === "function") {
                callback(refreshedEntry, message, watermarkResult || {});
            }
            return refreshedEntry;
        };

        if (root.permanentPdfWatermarkFilterEnabled(localFileName)) {
            root.requestPermanentWatermarkRemoval(localFileName, "refresh", result => {
                if (result && result.success && result.entry) {
                    if (typeof callback === "function") {
                        callback(result.entry, result.message || i18n("PDF replaced with watermark-free version."), result);
                    }
                    return;
                }

                refreshMetadata(result || {});
            });
            return null;
        }

        return refreshMetadata({});
    }

    function readOnlyEntryForFile(fileName, identifier) {
        return {
            filename: fileName,
            filetitle: String(fileName).split("/").pop(),
            title: fileTitleFromPath(fileName),
            uniqueIdentifier: identifier,
            zoomLevel: 1.0,
            pageMode: "",
            pdfNotInvertedRegions: ""
        };
    }

    function registerReadOnlyBook(fileName, callback) {
        const request = new XMLHttpRequest();
        request.open("POST", bookServerBaseUrl + "/read-only-books");
        request.setRequestHeader("Content-Type", "application/json");
        request.setRequestHeader("X-Arianna-Session-Token", bookServerSessionToken);
        request.onreadystatechange = function() {
            if (request.readyState !== 4) {
                return;
            }

            if (request.status < 200 || request.status >= 300) {
                console.warn("Unable to register read-only book", fileName, request.status, request.responseText);
                callback("");
                return;
            }

            const response = JSON.parse(request.responseText);
            callback(response.identifier || "");
        };
        request.send(JSON.stringify({
            fileName: fileName
        }));
    }

    function openReadOnlyBook(fileUrl, callback) {
        const fileName = localPathFromUrl(fileUrl);
        if (!fileName) {
            if (callback) {
                callback();
            }
            return;
        }

        registerReadOnlyBook(fileName, identifier => {
            const openBook = () => {
                if (identifier || isPdfFile(fileName)) {
                    openBookPage(fileName, "", "", readOnlyEntryForFile(fileName, identifier), true);
                }

                if (callback) {
                    callback();
                }
            };

            root.requestPermanentWatermarkRemoval(fileName, "open-read-only", openBook);
        });
    }

    function openReadOnlyBooks(fileUrls) {
        let index = 0;
        const openNext = () => {
            if (index >= fileUrls.length) {
                return;
            }

            const fileUrl = fileUrls[index];
            index += 1;
            openReadOnlyBook(fileUrl, openNext);
        };

        openNext();
    }

    function importBookEntry(filename) {
        const localFileName = localPathFromUrl(filename);
        const importedEntry = bookListModel.addBookFromFile(localFileName, true);
        if (importedEntry && importedEntry.filename) {
            root.requestPermanentWatermarkRemoval(importedEntry.filename, "import");
            return importedEntry;
        }

        const existingEntry = bookListModel.bookEntryFromFile(localFileName);
        if (existingEntry && existingEntry.filename) {
            return existingEntry;
        }

        return null;
    }

    function importBookFiles(fileUrls) {
        const paths = [];
        for (let i = 0; i < fileUrls.length; ++i) {
            paths.push(localPathFromUrl(fileUrls[i]));
        }
        const result = bookListModel.importBookFiles(paths, true);
        for (const path of result.importedFiles || []) {
            root.requestPermanentWatermarkRemoval(path, "import");
        }
        root.bookImportStatus = result.message || "";
        root.showStatusMessage(result.message);
        return result;
    }

    function showImportRejections() {
        root.importRejectionsPending = false;
        importRejectionDialog.open();
    }

    function reportImportRejection(rejection) {
        if (!importRejectionDialog.visible && !root.importRejectionsPending) {
            importRejectionDialog.rejections = [];
        }
        importRejectionDialog.rejections = [...importRejectionDialog.rejections, rejection];
        root.importRejectionsPending = true;
        Qt.callLater(root.showImportRejections);
    }

    function changeImportIdentifier(path) {
        const result = bookListModel.changeRejectedBookIdentifier(path);
        if (result.success) {
            importRejectionDialog.rejections = importRejectionDialog.rejections.filter(row => row.fileName !== path);
            if (importRejectionDialog.rejections.length === 0) importRejectionDialog.close();
            root.showStatusMessage(result.message);
        } else {
            importRejectionDialog.rejections = importRejectionDialog.rejections.map(row => row.fileName === path
                ? Object.assign({}, row, {canChangeIdentifier: result.canChangeIdentifier === true, actionMessage: result.message}) : row);
        }
    }

    function showStatusMessage(message) {
        const text = String(message || "");
        if (text.length === 0) {
            return;
        }

        try {
            root.showPassiveNotification(text);
        } catch (error) {
            console.log(text);
        }
    }

    function startCalibreLibraryImport() {
        if (root.calibreImportRunning) {
            return;
        }
        const folder = String(Config.calibreLibraryFolder || "").trim();
        if (folder.length === 0) {
            root.showStatusMessage(i18n("Set the Calibre library folder in Settings first."));
            Navigation.openSettings();
            return;
        }

        if (!root.bookListModel.cacheLoaded) {
            root.showStatusMessage(i18n("The library is still loading."));
            return;
        }

        root.calibreImportRunning = true;
        root.bookImportStatus = "";
        Qt.callLater(() => {
            const result = root.bookListModel.importCalibreLibrary(folder, true);
            root.calibreImportRunning = false;
            root.bookImportStatus = result && result.message ? result.message : i18n("Calibre import failed.");
            root.showStatusMessage(root.bookImportStatus);
            root.ensureLibraryPageVisible();
        });
    }

    function openBookPage(filename, locations, currentLocation, entry, readOnly, sourceTitle = "", initialSearch = null, noLibraryImport = false) {
        let bookEntry = entry;
        let bookLocations = locations;
        let bookCurrentLocation = currentLocation;
        let bookZoomLevel = bookEntry && bookEntry.zoomLevel > 0 ? bookEntry.zoomLevel : 1.0;
        let bookPageMode = bookEntry && bookEntry.pageMode ? bookEntry.pageMode : "";
        let bookPdfNotInvertedRegions = bookEntry && bookEntry.pdfNotInvertedRegions ? bookEntry.pdfNotInvertedRegions : "";
        const openReadOnly = readOnly === true;
        const skipLibraryImport = noLibraryImport === true;
        const openedFromReference = sourceTitle !== undefined && sourceTitle !== null && String(sourceTitle).length > 0;
        let latestLocation = bookCurrentLocation;
        let latestProgress = 0;
        let lastOpenedTimeFilename = "";

        if (filename && !openReadOnly && !skipLibraryImport && (!bookEntry || !bookEntry.filename)) {
            const importedEntry = importBookEntry(filename);
            if (importedEntry) {
                bookEntry = importedEntry;
                filename = importedEntry.filename || filename;
                bookLocations = importedEntry.locations;
                bookCurrentLocation = importedEntry.currentLocation;
                bookZoomLevel = importedEntry.zoomLevel > 0 ? importedEntry.zoomLevel : 1.0;
                bookPageMode = importedEntry.pageMode || "";
                bookPdfNotInvertedRegions = importedEntry.pdfNotInvertedRegions || "";
            } else {
                return null;
            }
        }

        if (filename && !openReadOnly && !skipLibraryImport && bookEntry && bookEntry.filename && !bookEntry.uniqueIdentifier && !bookEntry.identifier) {
            const refreshedEntry = bookListModel.refreshBookFromFile(filename, false);
            if (refreshedEntry && refreshedEntry.filename) {
                bookEntry = refreshedEntry;
                filename = refreshedEntry.filename || filename;
                bookLocations = bookLocations || refreshedEntry.locations;
                bookCurrentLocation = bookCurrentLocation || refreshedEntry.currentLocation;
                bookZoomLevel = refreshedEntry.zoomLevel > 0 ? refreshedEntry.zoomLevel : bookZoomLevel;
                bookPageMode = refreshedEntry.pageMode || bookPageMode;
                bookPdfNotInvertedRegions = refreshedEntry.pdfNotInvertedRegions || bookPdfNotInvertedRegions;
            }
        }

        const isPdf = root.isPdfFile(filename);
        const viewerPage = isPdf ? './PdfViewerPage.qml' : './EpubViewerPage.qml';
        const bookIdentifier = bookEntry ? (bookEntry.uniqueIdentifier || bookEntry.identifier || "") : "";
        const bookUrl = isPdf && bookIdentifier.length > 0
            ? bookServerBaseUrl + "/" + encodeURIComponent(bookIdentifier) + "/book.pdf?token=" + encodeURIComponent(bookServerSessionToken)
            : root.fileUrlFromLocalPath(filename);
        const deferBookLoadUntilAfterLayerPush = openedFromReference && !isPdf;
        if (!isPdf) {
            Navigation.ensureWebEngineInitialized();
        }

        const viewerProperties = {
            currentLocation: bookCurrentLocation,
            locations: bookLocations,
            zoomLevel: bookZoomLevel,
            pageMode: bookPageMode,
            pageWidthDistance: bookEntry && bookEntry.pageWidthDistance >= 0 ? bookEntry.pageWidthDistance : -1,
            filename: filename,
            entry: bookEntry,
            readOnly: openReadOnly,
            referenceSourceTitle: sourceTitle
        };
        if (!isPdf) viewerProperties.noLibraryImport = skipLibraryImport;
        if (isPdf) {
            viewerProperties.pdfNotInvertedRegions = bookPdfNotInvertedRegions;
        }
        if (initialSearch) {
            viewerProperties.searchText = initialSearch.query;
            viewerProperties.initialSearch = initialSearch;
        }
        // url change is a loading trigger so put this last
        viewerProperties.url = deferBookLoadUntilAfterLayerPush ? "" : bookUrl;

        const previousLayer = root.pageStack.layers.currentItem;
        const previousReader = previousLayer && previousLayer.item ? previousLayer.item : previousLayer;
        if (previousReader && typeof previousReader.suspendReaderForReference === "function" && !previousReader.readerSuspended) {
            previousReader.suspendReaderForReference("open-book-page");
        }
        const bookViewerLayer = root.pageStack.layers.push(viewerPage, viewerProperties);
        const bookViewer = bookViewerLayer && bookViewerLayer.item ? bookViewerLayer.item : bookViewerLayer;
        let saveLocationTimer = null;
        let deferredBookLoadStarted = false;

        function startDeferredBookLoad(viewer) {
            if (!deferBookLoadUntilAfterLayerPush || deferredBookLoadStarted || !viewer) {
                return;
            }

            deferredBookLoadStarted = true;
            Qt.callLater(() => {
                if (!viewer || typeof viewer.url === "undefined" || viewer.url) {
                    return;
                }

                console.log("opening referenced book at referenced location",
                            "stage:", "qml-deferred-url-after-layer-push",
                            "location:", bookCurrentLocation || "");
                viewer.url = bookUrl;
            });
        }

        function ensureSaveLocationTimer() {
            if (saveLocationTimer) {
                return saveLocationTimer;
            }

            saveLocationTimer = Qt.createQmlObject('import QtQuick\nTimer { interval: 1000; repeat: false }', root, 'readerLocationSaveTimer');
            saveLocationTimer.triggered.connect(flushReadingPosition);
            return saveLocationTimer;
        }

        function currentViewerFilename() {
            const currentViewer = bookViewerLayer && bookViewerLayer.item ? bookViewerLayer.item : bookViewer;
            const currentFilename = currentViewer && currentViewer.filename ? currentViewer.filename : filename;
            return root.localPathFromUrl(currentFilename || "");
        }

        function flushReadingPosition() {
            const currentFilename = currentViewerFilename();
            if (!bookViewer || bookViewer.readOnly || bookViewer.noLibraryImport || !latestLocation || !currentFilename) {
                return;
            }

            bookListModel.setBookData(currentFilename, 'currentLocation', latestLocation);
            bookListModel.setBookData(currentFilename, 'currentProgress', latestProgress);
        }

        function scheduleReadingPositionSave() {
            if (!bookViewer || bookViewer.readOnly || bookViewer.noLibraryImport) {
                return;
            }

            ensureSaveLocationTimer().restart();
        }

        function destroySaveLocationTimer() {
            if (!saveLocationTimer) {
                return;
            }

            saveLocationTimer.stop();
            saveLocationTimer.destroy();
            saveLocationTimer = null;
        }

        function updateLastOpenedTime() {
            const currentFilename = currentViewerFilename();
            if (!bookViewer || bookViewer.readOnly || bookViewer.noLibraryImport || !currentFilename || lastOpenedTimeFilename === currentFilename) {
                return;
            }

            lastOpenedTimeFilename = currentFilename;
            bookListModel.setBookData(currentFilename, 'lastOpenedTime', new Date().toISOString());
        }

        function connectBookViewerSignals(viewer) {
            if (!viewer || typeof viewer.relocated === 'undefined') {
                return;
            }

            // A covered/suspended page remains open. ReaderHistory records only
            // page destruction and application exit, not bookClosed/visibility.
            function trackLibraryReader() {
                if (!viewer.readOnly && !viewer.noLibraryImport) {
                    readerHistory.trackReader(viewer, currentViewerFilename());
                }
            }
            trackLibraryReader();
            viewer.filenameChanged.connect(trackLibraryReader);

            function syncCurrentViewerPosition() {
                if (!viewer || viewer.readOnly || viewer.noLibraryImport || !viewer.currentLocation) {
                    return;
                }

                latestLocation = viewer.currentLocation;
                if (typeof viewer.currentProgress !== "undefined") {
                    latestProgress = viewer.currentProgress;
                }
                scheduleReadingPositionSave();
            }

            viewer.relocated.connect((newLocation, newProgress) => {
                latestLocation = newLocation;
                latestProgress = newProgress;
                scheduleReadingPositionSave();
            });

            viewer.locationsLoaded.connect(locations => {
                const currentFilename = currentViewerFilename();
                if (!viewer.readOnly && !viewer.noLibraryImport && currentFilename) {
                    bookListModel.setBookData(currentFilename, 'locations', locations);
                }
            });

            viewer.zoomLevelSaved.connect(zoomLevel => {
                const currentFilename = currentViewerFilename();
                if (!viewer.readOnly && !viewer.noLibraryImport && currentFilename) {
                    bookListModel.setBookData(currentFilename, 'zoomLevel', zoomLevel);
                }
            });

            viewer.pageWidthDistanceSaved.connect(value => {
                const currentFilename = currentViewerFilename();
                if (!viewer.readOnly && !viewer.noLibraryImport && currentFilename) {
                    bookListModel.setBookData(currentFilename, 'pageWidthDistance', value);
                }
            });

            if (typeof viewer.pageModeSaved !== 'undefined') {
                viewer.pageModeSaved.connect(pageMode => {
                    const currentFilename = currentViewerFilename();
                    if (!viewer.readOnly && !viewer.noLibraryImport && currentFilename) {
                        bookListModel.setBookData(currentFilename, 'pageMode', pageMode);
                    }
                });
            }

            if (typeof viewer.pdfNotInvertedRegionsSaved !== 'undefined') {
                viewer.pdfNotInvertedRegionsSaved.connect(regions => {
                    const currentFilename = currentViewerFilename();
                    if (!viewer.readOnly && !viewer.noLibraryImport && currentFilename) {
                        bookListModel.setBookData(currentFilename, 'pdfNotInvertedRegions', regions);
                    }
                });
            }

            viewer.addToLibraryRequested.connect(callback => {
                const currentFilename = currentViewerFilename();
                const importedEntry = importBookEntry(currentFilename);
                if (!importedEntry) {
                    if (typeof callback === "function") {
                        callback(null);
                    }
                    return;
                }

                viewer.entry = importedEntry;
                viewer.filename = importedEntry.filename || currentFilename;
                viewer.readOnly = false;
                if (typeof viewer.noLibraryImport !== "undefined") viewer.noLibraryImport = false;
                trackLibraryReader();
                viewer.locations = importedEntry.locations;
                if (!viewer.pdfNotInvertedRegions && importedEntry.pdfNotInvertedRegions) {
                    viewer.pdfNotInvertedRegions = importedEntry.pdfNotInvertedRegions;
                }

                if (!viewer.currentLocation && importedEntry.currentLocation) {
                    viewer.currentLocation = importedEntry.currentLocation;
                }

                updateLastOpenedTime();
                if (latestLocation) {
                    bookListModel.setBookData(viewer.filename, 'currentLocation', latestLocation);
                }
                if (latestProgress > 0) {
                    bookListModel.setBookData(viewer.filename, 'currentProgress', latestProgress);
                }
                if (viewer.locations) {
                    bookListModel.setBookData(viewer.filename, 'locations', viewer.locations);
                }
                if (viewer.zoomLevel > 0) {
                    bookListModel.setBookData(viewer.filename, 'zoomLevel', viewer.zoomLevel);
                }
                if (viewer.pageWidthDistance >= 0) {
                    bookListModel.setBookData(viewer.filename, 'pageWidthDistance', viewer.pageWidthDistance);
                }
                if (typeof viewer.currentPageMode === "function") {
                    bookListModel.setBookData(viewer.filename, 'pageMode', viewer.currentPageMode());
                } else if (typeof viewer.pageMode !== "undefined" && String(viewer.pageMode).length > 0) {
                    bookListModel.setBookData(viewer.filename, 'pageMode', viewer.pageMode);
                }
                if (typeof viewer.pdfNotInvertedRegions !== "undefined" && String(viewer.pdfNotInvertedRegions).length > 0) {
                    bookListModel.setBookData(viewer.filename, 'pdfNotInvertedRegions', viewer.pdfNotInvertedRegions);
                }
                if (typeof callback === "function") {
                    callback(importedEntry);
                }
            });

            viewer.bookReady.connect(title => {
                root.title = title;
                updateLastOpenedTime();
                syncCurrentViewerPosition();
            });

            viewer.bookClosed.connect(() => {
                flushReadingPosition();
                destroySaveLocationTimer();
                root.ensureLibraryPageVisible();
                root.updateOverviewWindowTitle();
                if (openedFromReference) {
                    Qt.callLater(root.reloadCurrentReaderPage);
                }
            });

            if (viewer.documentReady === true) {
                root.title = viewer.displayTitle || root.title;
                updateLastOpenedTime();
                syncCurrentViewerPosition();
            }
            startDeferredBookLoad(viewer);
        }

        if (!bookViewer || typeof bookViewer.relocated === 'undefined') {
            console.error('Book viewer page is not available yet; deferring signal connection', bookViewerLayer);
            Qt.callLater(() => {
                const deferredViewer = bookViewerLayer && bookViewerLayer.item ? bookViewerLayer.item : bookViewerLayer;
                if (deferredViewer && typeof deferredViewer.relocated !== 'undefined') {
                    connectBookViewerSignals(deferredViewer);
                } else {
                    console.error('Book viewer still not ready after deferral', bookViewerLayer);
                }
            });
            return;
        }

        connectBookViewerSignals(bookViewer);
    }

    function openReferencePage(filename, currentLocation, entry, readOnly, locations, sourceTitle) {
        console.log("opening referenced book at referenced location",
                    "filename:", filename || "",
                    "bookId:", entry ? (entry.uniqueIdentifier || entry.identifier || "") : "",
                    "location:", currentLocation || "",
                    "readOnly:", readOnly === true);
        const sourceReader = root.pageStack.layers.currentItem;
        if (sourceReader && typeof sourceReader.suspendReaderForReference === "function") {
            sourceReader.suspendReaderForReference("open-reference-page");
        }
        openBookPage(filename, locations, currentLocation, entry, readOnly, sourceTitle);
    }

    function startLibraryFullTextSearch(query) {
        if (!root.bookListModel.cacheLoaded || libraryFullTextSearch.running || query.trim().length === 0) {
            return;
        }
        libraryFullTextSearch.start(query);
        root.showLibraryFullTextSearchResults();
    }

    function showLibraryFullTextSearchResults() {
        goHomeButton.checked = false;
        if (!root.pageStack.currentItem || root.pageStack.currentItem.librarySearchPage !== true) {
            root.pageStack.replace(librarySearchResultsPage);
        }
        root.updateOverviewWindowTitle();
    }

    function showHomeLibraryPage() {
        root.readerFullScreen = false;
        root.pageStack.replace(Qt.resolvedUrl('./LibraryPage.qml'), {
            pageTitle: i18n("Library"),
            bookListModel: root.bookListModel,
            addBookAction: addBookAction
        });
        root.updateOverviewWindowTitle();
    }

    function ensureLibraryPageVisible() {
        const currentPage = root.pageStack.currentItem;
        if (currentPage && currentPage.startupPlaceholder === true) {
            root.showHomeLibraryPage();
        }
    }

    BookEditor {
        id: maintenanceEditor
    }

    function editMaintenanceFile(fileName, bookId, cfi, selectedText, completion) {
        const entry = bookId ? ({ uniqueIdentifier: bookId }) : null;
        const result = maintenanceEditor.openBook(fileName, entry, cfi || "", selectedText || "");
        if (completion) {
            completion(result);
        }
    }

    function reloadCurrentReaderPage() {
        const currentLayer = root.pageStack.layers.currentItem;
        if (currentLayer && typeof currentLayer.reloadCurrentBook === "function") {
            currentLayer.reloadCurrentBook();
            return;
        }

        const currentPage = root.pageStack.currentItem;
        if (currentPage && typeof currentPage.reloadCurrentBook === "function") {
            currentPage.reloadCurrentBook();
        }
    }

    // Transient BookServer registrations have to survive an explicit restart.
    function readerSessionFiles() {
        const files = [];
        for (let i = 1; i < root.pageStack.layers.depth; ++i) {
            const layer = root.pageStack.layers.get(i);
            const page = layer && layer.item ? layer.item : layer;
            if (page && page.filename && (page.readOnly || page.noLibraryImport)) {
                files.push({fileName: root.localPathFromUrl(page.filename),
                    identifier: page.entry ? (page.entry.uniqueIdentifier || page.entry.identifier || "") : ""});
            }
        }
        return JSON.stringify(files);
    }

    function refreshReaderSessions() {
        let count = 0;
        for (let i = 1; i < root.pageStack.layers.depth; ++i) {
            const layer = root.pageStack.layers.get(i);
            const page = layer && layer.item ? layer.item : layer;
            if (!page || typeof page.reloadCurrentBook !== "function") continue;
            if (typeof page.suspendReaderForReference === "function" && !page.activeLayerPage) {
                // Keep covered readers suspended; their next activation reloads them.
                page.suspendReaderForReference("refresh-reader-session");
            } else {
                page.reloadCurrentBook();
            }
            ++count;
        }
        console.info("Refreshed reader sessions:", count);
        return count;
    }

    function openAddBooksDialog() {
        addBooksDialog.open();
    }

    function startBalooImportSearch() {
        root.showImportPage("baloo");
        if (!balooImportModel.balooAvailable) {
            return;
        }
        if (root.balooImportRunning || !root.bookListModel.cacheLoaded) {
            return;
        }
        root.balooImportSearched = false;
        root.bookImportStatus = "";
        root.balooImportRunning = true;
        balooImportModel.setIgnoredFiles(root.bookListModel.knownBookFiles());
        balooImportModel.startSearch();
    }

    function importBalooSearchResults() {
        const filePaths = balooImportModel.filePaths();
        const result = root.importBookFiles(filePaths);
        balooImportModel.setIgnoredFiles(root.bookListModel.knownBookFiles());
        return result;
    }

    width: Kirigami.Units.gridUnit * 65
    height: Kirigami.Units.gridUnit * 45
    minimumWidth: Kirigami.Units.gridUnit * 20
    minimumHeight: Kirigami.Units.gridUnit * 30

    Component {
        id: startupPlaceholderPage

        Kirigami.Page {
            readonly property bool startupPlaceholder: true
            readonly property bool hideSidebar: true
            readonly property bool centerToolbarActions: true
        }
    }

    Component {
        id: libraryInitialPage

        LibraryPage {
            pageTitle: i18n("Library")
            bookListModel: root.bookListModel
            addBookAction: addBookAction
        }
    }

    LibrarySearchModel {
        id: libraryFullTextSearch
        objectName: "libraryFullTextSearchModel"
    }

    Component {
        id: librarySearchResultsPage

        LibrarySearchPage {
            searchModel: libraryFullTextSearch
            onTitleChanged: root.updateOverviewWindowTitle()
            onOpenResult: (filename, location, query, indexAtLocation) => {
                const entry = root.bookListModel.bookEntryFromFile(filename);
                if (!entry || !entry.filename) {
                    root.showStatusMessage(i18n("This book is no longer in the library."));
                    return;
                }
                root.openBookPage(filename, entry.locations, location, entry, false, "", {
                    query: query,
                    location: location,
                    indexAtLocation: indexAtLocation
                });
            }
        }
    }

    pageStack {
        defaultColumnWidth: Kirigami.Units.gridUnit * 30
        onCurrentItemChanged: root.updateOverviewWindowTitle()
        globalToolBar {
            canContainHandles: true
            style: Kirigami.ApplicationHeaderStyle.ToolBar
            showNavigationButtons: applicationWindow().pageStack.currentIndex > 0 ? Kirigami.ApplicationHeaderStyle.ShowBackButton : 0
            toolbarActionAlignment: {
                const currentPage = root.pageStack.layers.depth > 1 ? root.pageStack.layers.currentItem : root.pageStack.currentItem;
                return root.centerToolbarActionsFor(currentPage) ? Qt.AlignHCenter : Qt.AlignRight;
            }
        }

        initialPage: root.directStartupReader ? startupPlaceholderPage : libraryInitialPage
    }

    readonly property BookListModel bookListModel: BookListModel {
        onCountChanged: root.updateOverviewWindowTitle()
        onStructureReviewRequested: bookId => root.showWorkStructure(bookId)
        onImportRejected: rejection => root.reportImportRejection(rejection)
    }

    ContentList {
        id: balooImportModel
        objectName: "balooImportModel"

        autoSearch: false
        cacheResults: false

        onSearchStarted: {
            root.isLoading = true;
            root.balooImportRunning = true;
        }
        onSearchCompleted: {
            root.isLoading = false;
            root.balooImportRunning = false;
            root.balooImportSearched = true;
        }

        ContentQuery {
            type: ContentQuery.Epub
            mimeTypes: ["application/epub+zip", "application/pdf"]
            locations: Config.bookLocations
        }
    }

    globalDrawer: Kirigami.OverlayDrawer {
        id: navigationDrawer

        edge: Qt.application.layoutDirection === Qt.RightToLeft ? Qt.RightEdge : Qt.LeftEdge
        modal: Kirigami.Settings.isMobile || (applicationWindow().width < Kirigami.Units.gridUnit * 50 && !collapsed) // Only modal when not collapsed, otherwise collapsed won't show.
        z: modal ? Math.round(position * 10000000) : 100
        collapsible: !modal && !Kirigami.Settings.isMobile
        collapsedSize: Kirigami.Units.gridUnit * 3
        drawerOpen: !Kirigami.Settings.isMobile && enabled
        enabled: !root.readerFullScreen && pageStack.currentItem && pageStack.currentItem.hideSidebar !== true && (!pageStack.layers.currentItem || pageStack.layers.currentItem.hideSidebar !== true)
        onEnabledChanged: drawerOpen = !Kirigami.Settings.isMobile && enabled
        preferredSize: Kirigami.Units.gridUnit * 16
        Behavior on implicitWidth {
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
        onModalChanged: if (!modal && (!pageStack.layers.currentItem || pageStack.layers.currentItem.hideSidebar !== true)) {
            drawerOpen = true;
        }

        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: 0

        contentItem: ColumnLayout {
            spacing: 0

            QQC2.ButtonGroup {
                id: placeGroup
            }

            QQC2.ScrollView {
                id: scrollView

                Layout.fillHeight: true
                Layout.fillWidth: true

                contentWidth: availableWidth
                topPadding: Kirigami.Units.smallSpacing / 2

                ColumnLayout {
                    spacing: 0
                    width: scrollView.width

                    PlaceItem {
                        id: goHomeButton
                        objectName: "goHomeButton"
                        fullText: i18nc("Switch to the listing page showing the most recently read books, %1 is the number of books", "Home (%1)", root.bookListModel.count)
                        icon.name: "go-home"
                        checked: true
                        QQC2.ButtonGroup.group: placeGroup
                        onTriggered: Navigation.openLibrary(i18n("Library"), bookListModel, true)
                    }

                    PlaceItem {
                        objectName: "libraryFullTextSearchMenuItem"
                        fullText: i18nc("@title:group", "Full-text Search")
                        icon.name: "edit-find"
                        enabled: root.bookListModel.cacheLoaded
                        checked: root.pageStack.currentItem && root.pageStack.currentItem.librarySearchPage === true
                        QQC2.ButtonGroup.group: placeGroup
                        onClicked: {
                            navigationDrawer.collapsed = false;
                            root.showLibraryFullTextSearchResults();
                            librarySearchPane.focusSearch();
                        }
                    }

                    LibrarySearchPane {
                        id: librarySearchPane
                        objectName: "libraryFullTextSearchPane"
                        Layout.fillWidth: true
                        Layout.leftMargin: Kirigami.Units.smallSpacing
                        Layout.rightMargin: Kirigami.Units.smallSpacing
                        Layout.topMargin: Kirigami.Units.smallSpacing
                        visible: !navigationDrawer.collapsed
                        libraryReady: root.bookListModel.cacheLoaded
                        searchModel: libraryFullTextSearch
                        onSearchRequested: query => root.startLibraryFullTextSearch(query)
                    }

                    DrawerSeparator {}

                    PlaceItem {
                        id: maintenanceButton
                        objectName: "maintenancePageButton"
                        fullText: i18nc("@title:group", "Maintenance")
                        icon.name: "tools"
                        enabled: root.bookListModel.cacheLoaded
                        checked: root.pageStack.currentItem && root.pageStack.currentItem.maintenancePage === true
                        QQC2.ButtonGroup.group: placeGroup
                        onClicked: root.showMaintenancePage()
                    }

                    DrawerSeparator {}

                    PlaceItem {
                        objectName: "bookImportPageButton"
                        fullText: i18nc("@title:group", "Book Import")
                        icon.name: "document-import"
                        enabled: root.bookListModel.cacheLoaded
                        checked: root.pageStack.currentItem && root.pageStack.currentItem.importPage === true
                        QQC2.ButtonGroup.group: placeGroup
                        onClicked: root.showImportPage()
                    }

                    DrawerSeparator {}

                    PlaceItem {
                        fullText: i18nc("@action:button", "Open without Import...")
                        icon.name: "document-open"
                        onClicked: openOtherBooksDialog.open()
                        QQC2.ButtonGroup.group: null
                        checkable: false
                    }

                    DrawerSeparator {}

                    PlaceItem {
                        fullText: i18nc("Open the settings page", "Settings")
                        icon.name: "configure"
                        onClicked: Navigation.openSettings()
                        QQC2.ButtonGroup.group: placeGroup
                        checkable: false
                        Layout.bottomMargin: Kirigami.Units.smallSpacing / 2
                    }

                }
            }

            Item {
                Layout.fillHeight: true
            }

            QQC2.ToolButton {
                Layout.fillWidth: true
                visible: navigationDrawer.collapsible
                display: navigationDrawer.collapsed ? QQC2.AbstractButton.IconOnly : QQC2.AbstractButton.TextBesideIcon
                icon.name: {
                    const mirrored = navigationDrawer.edge === Qt.RightEdge;
                    if (navigationDrawer.collapsed) {
                        return mirrored ? "sidebar-expand-right" : "sidebar-expand-left";
                    }
                    return mirrored ? "sidebar-collapse-right" : "sidebar-collapse-left";
                }
                text: navigationDrawer.collapsed ? "" : i18nc("@action:button", "Collapse Sidebar")
                onClicked: navigationDrawer.collapsed = !navigationDrawer.collapsed

                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.text: navigationDrawer.collapsed ? i18nc("@info:tooltip", "Expand Sidebar") : i18nc("@info:tooltip", "Collapse Sidebar")
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }
    }

    component PlaceItem: Delegates.RoundedItemDelegate {
        id: item

        property string fullText: ""

        signal triggered
        checkable: true
        Layout.fillWidth: true
        display: root.globalDrawer && root.globalDrawer.collapsed ? QQC2.AbstractButton.IconOnly : QQC2.AbstractButton.TextBesideIcon
        text: root.globalDrawer && root.globalDrawer.collapsed ? "" : fullText
        Keys.onDownPressed: nextItemInFocusChain().forceActiveFocus(Qt.TabFocusReason)
        Keys.onUpPressed: nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocusReason)
        Accessible.role: Accessible.MenuItem
        Accessible.name: fullText
        highlighted: checked || activeFocus
        onToggled: if (checked) {
            item.triggered();
        }
        QQC2.ToolTip.visible: root.globalDrawer && root.globalDrawer.collapsed && hovered && fullText.length > 0
        QQC2.ToolTip.text: fullText
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
    }

    component DrawerSectionHeader: QQC2.Label {
        Layout.fillWidth: true
        Layout.leftMargin: Kirigami.Units.gridUnit
        Layout.rightMargin: Kirigami.Units.gridUnit
        Layout.topMargin: Kirigami.Units.smallSpacing
        Layout.bottomMargin: Kirigami.Units.smallSpacing / 2
        visible: !(root.globalDrawer && root.globalDrawer.collapsed)
        color: Kirigami.Theme.disabledTextColor
        elide: Text.ElideRight
        font.family: Kirigami.Theme.smallFont.family
        font.pixelSize: Kirigami.Theme.smallFont.pixelSize
        font.bold: true
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    component DrawerSeparator: Rectangle {
        Layout.fillWidth: true
        Layout.leftMargin: Kirigami.Units.smallSpacing
        Layout.rightMargin: Kirigami.Units.smallSpacing
        Layout.topMargin: Kirigami.Units.smallSpacing / 2
        Layout.bottomMargin: Kirigami.Units.smallSpacing / 2
        implicitHeight: 1
        color: Kirigami.Theme.disabledTextColor
        opacity: 0.35
    }

    contextDrawer: TableOfContentDrawer {
        modal: !root.wideScreen || !enabled
        onEnabledChanged: drawerOpen = enabled && !modal
        enabled: root.pageStack.layers.depth > 1
        handleVisible: enabled
    }

    Connections {
        target: Navigation

        function onOpenBook(filename, locations, currentLocation, entry, readOnly, noLibraryImport) {
            root.openBookPage(filename, locations, currentLocation, entry, readOnly, "", null, noLibraryImport);
        }

        function onOpenLibrary(title, model, replace) {
            root.readerFullScreen = false;

            const currentPage = root.pageStack.currentItem;
            if (!currentPage || currentPage.libraryPage !== true) {
                root.pageStack.replace(Qt.resolvedUrl('./LibraryPage.qml'), {
                    pageTitle: title,
                    bookListModel: model,
                    addBookAction: addBookAction
                });
                root.pageStack.currentItem.pageTitle = title;
                root.pageStack.currentItem.bookListModel = model;
                return;
            }
            if (replace) {
                while (root.pageStack.depth > 1) {
                    root.pageStack.pop();
                }
                root.pageStack.currentItem.pageTitle = title;
                root.pageStack.currentItem.bookListModel = model;
                return;
            }

            root.pageStack.push(Qt.resolvedUrl('./LibraryPage.qml'), {
                pageTitle: title,
                bookListModel: model,
                addBookAction: addBookAction
            });
        }

        function onOpenSettings() {
            pageStack.pushDialogLayer(Qt.resolvedUrl('./SettingsPage.qml'), {}, {
                title: i18n("Settings"),
                width: Kirigami.Units.gridUnit * 24
            });
        }
    }

    QQC2.Dialog {
        id: permanentWatermarkRemovalDialog

        parent: QQC2.Overlay.overlay ? QQC2.Overlay.overlay : root
        modal: true
        title: i18nc("@title:window", "Remove PDF Watermark")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel

        onAccepted: {
            const request = root.activeWatermarkRemovalRequest;
            if (!request) {
                root.finishActiveWatermarkRemoval({});
                return;
            }

            const result = root.bookListModel.replaceBookFileWithWatermarkFreePdf(request.fileName, request.replacementFileName);
            root.finishActiveWatermarkRemoval(result);
        }

        onRejected: {
            const request = root.activeWatermarkRemovalRequest;
            if (request && request.replacementFileName && root.bookListModel) {
                root.bookListModel.discardWatermarkFreePdf(request.replacementFileName);
            }
            root.finishActiveWatermarkRemoval({
                declined: true,
                message: i18n("PDF watermark removal was canceled.")
            });
        }

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.largeSpacing
            implicitWidth: Kirigami.Units.gridUnit * 32

            QQC2.Label {
                Layout.fillWidth: true
                text: {
                    const request = root.activeWatermarkRemovalRequest || {};
                    const occurrences = request.occurrences || 0;
                    return occurrences === 1
                        ? i18n("One PDF watermark marker was found. Replace the original file with the watermark-free version?")
                        : i18n("%1 PDF watermark markers were found. Replace the original file with the watermark-free version?", occurrences);
                }
                wrapMode: Text.WordWrap
            }

            QQC2.Label {
                Layout.fillWidth: true
                text: root.activeWatermarkRemovalRequest ? root.activeWatermarkRemovalRequest.fileName : ""
                elide: Text.ElideMiddle
            }
        }
    }

    QQC2.Dialog {
        id: importRejectionDialog
        objectName: "importRejectionDialog"
        property var rejections: []
        parent: QQC2.Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: i18n("Files Not Imported")
        standardButtons: QQC2.Dialog.Close
        width: Math.min(root.width - Kirigami.Units.gridUnit * 2, Kirigami.Units.gridUnit * 48)
        height: Math.min(root.height - Kirigami.Units.gridUnit * 2, Kirigami.Units.gridUnit * 32)
        contentItem: ColumnLayout {
            QQC2.Label {
                Layout.fillWidth: true
                text: i18n("%1 files were not imported. The remaining files were processed.", importRejectionDialog.rejections.length)
                wrapMode: Text.Wrap
            }
            QQC2.ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                ListView {
                    objectName: "importRejectionDetails"
                    clip: true
                    model: importRejectionDialog.rejections
                    spacing: Kirigami.Units.largeSpacing
                    delegate: ColumnLayout {
                        required property var modelData
                        width: ListView.view.width
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: modelData.message
                            textFormat: Text.PlainText
                            wrapMode: Text.WrapAnywhere
                        }
                        QQC2.Button {
                            objectName: "changeRejectedBookIdentifier"
                            readonly property string fileName: modelData.fileName
                            visible: modelData.canChangeIdentifier === true
                            text: i18nc("@action:button", "Change Book Identifier")
                            icon.name: "edit-rename"
                            onClicked: root.changeImportIdentifier(fileName)
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            visible: modelData.canChangeIdentifier === true
                            text: i18n("Assigns a new book ID to this file and retries the import.")
                            wrapMode: Text.Wrap
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            visible: !!modelData.actionMessage
                            text: modelData.actionMessage || ""
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }
        }
    }

    FileDialog {
        id: addBooksDialog
        objectName: "addBooksDialog"

        title: i18n("Add Books")
        fileMode: FileDialog.OpenFiles
        nameFilters: [root.ebookFileNameFilter]

        onAccepted: {
            const selectedBookFiles = files.length > 0 ? files : (file ? [file] : []);
            root.importBookFiles(selectedBookFiles);
        }
    }

    FileDialog {
        id: openOtherBooksDialog

        title: i18n("Open without Import...")
        fileMode: FileDialog.OpenFiles
        nameFilters: [root.ebookFileNameFilter]

        onAccepted: {
            const selectedBookFiles = files.length > 0 ? files : (file ? [file] : []);
            root.openReadOnlyBooks(selectedBookFiles);
        }
    }

    Kirigami.Action {
        id: addBookAction
        text: i18nc("@action:button", "Add Books…")
        icon.name: "list-add"
        onTriggered: root.openAddBooksDialog()
    }

    Kirigami.Action {
        id: openOtherAction
        text: i18nc("@action:button", "Open without Import...")
        icon.name: "document-open"
        onTriggered: openOtherBooksDialog.open()
    }

    KConfig.WindowStateSaver {
        configGroupName: "Main"
    }
}
