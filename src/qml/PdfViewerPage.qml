// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.arianna
import org.kde.kirigami as Kirigami

Kirigami.Page {
    id: root

    property string url: ""
    property string filename: ""
    property string locations: ""
    property string currentLocation: ""
    property real zoomLevel: 1.0
    property string pageMode: ""
    property string pdfNotInvertedRegions: ""
    property var entry: null
    property bool readOnly: false
    property string referenceSourceTitle: ""
    property bool initialLocationApplied: false
    property bool restoringInitialLocation: false
    property bool searchVisible: false
    property bool twoPageMode: false
    property bool notInvertedRegionSelectionMode: false
    property bool ovalNotInvertedRegionSelectionMode: false
    property bool forcedInvertedRegionSelectionMode: false
    property bool translationPending: false
    property int currentReaderTheme: Config.readerTheme
    property int pdfNotInvertedRegionsVersion: 0
    property var pdfNotInvertedRegionData: ({
            pages: ({})
        })
    property var pendingNotInvertedRegion: null
    property var pendingForcedInvertedRegion: null
    property var tableOfContentsEntries: []
    property var manualTableOfContentsEntries: []
    property int manualTableOfContentsPage: -1
    property var primaryTableOfContentsEntries: []
    property var primaryTableOfContentsPages: []
    property var pdfBackHistory: []
    property var pdfForwardHistory: []
    property bool pdfCanGoBack: false
    property bool pdfCanGoForward: false
    property string pdfHistoryCurrentLocation: ""
    readonly property var pdfDocument: document
    readonly property bool hideSidebar: true
    readonly property bool centerToolbarActions: true
    readonly property bool documentReady: document.opened
    readonly property string defaultPageMode: Config.readerPageMode === 0 ? "single" : "two"
    readonly property string effectivePageMode: root.pageMode === "single" || root.pageMode === "two" ? root.pageMode : root.defaultPageMode
    readonly property bool initialTwoPageMode: root.effectivePageMode === "two"
    readonly property int activePage: view.currentPage
    readonly property int readingPage: root.pageForReadingPosition()
    readonly property int currentProgress: progressForPage(root.readingPage)
    readonly property bool coverPageMode: root.twoPageMode && root.hasCoverPage()
    readonly property bool pdfPageInverted: root.currentReaderTheme === 1
    readonly property bool pdfPageUsesSystemColors: root.currentReaderTheme === 2
    readonly property int activeSearchResultCount: view.searchModel.count
    readonly property real scaleStep: Math.pow(2, 1 / 8)
    readonly property int maximumPdfHistoryEntries: 100
    readonly property string filetitle: {
        const value = root.filename || root.url;
        const fileName = String(value).split("/").pop();
        const extensionIndex = fileName.lastIndexOf(".");
        return extensionIndex > 0 ? fileName.substring(0, extensionIndex) : fileName;
    }
    readonly property string displayTitle: document.title || (root.entry ? root.entry.title || "" : "") || root.filetitle

    signal relocated(newLocation: var, newProgress: int)
    signal locationsLoaded(locations: var)
    signal zoomLevelSaved(zoomLevel: real)
    signal pageModeSaved(pageMode: string)
    signal pdfNotInvertedRegionsSaved(regions: string)
    signal bookReady(title: var)
    signal bookClosed
    signal addToLibraryRequested(var callback)

    title: root.displayTitle + (root.referenceSourceTitle ? " <- " + root.referenceSourceTitle : "")
    padding: 0

    function clampedScale(scale) {
        return Math.max(0.1, Math.min(10, scale));
    }

    function setZoom(scale) {
        const clampedScale = root.clampedScale(scale);
        if (Math.abs(root.zoomLevel - clampedScale) < 0.001) {
            return;
        }

        root.zoomLevel = clampedScale;
        root.updateReadingPosition();
        zoomSaveTimer.restart();
    }

    function adjustZoom(factor) {
        root.setZoom(root.zoomLevel * factor);
    }

    function setReaderThemeMode(mode) {
        if (root.currentReaderTheme === mode && Config.readerTheme === mode) {
            return;
        }

        root.currentReaderTheme = mode;
        Config.readerTheme = mode;
        Config.save();
    }

    function cycleReaderThemeMode() {
        root.setReaderThemeMode((root.currentReaderTheme + 1) % 3);
    }

    function readerBackgroundImageSource() {
        const path = String(Config.readerBackgroundPath || "").trim();
        if (path.length === 0) {
            return "";
        }
        if (path.indexOf(":") > 0 || path.startsWith("qrc:") || path.startsWith(":/")) {
            return path;
        }
        if (path.startsWith("/")) {
            return "file://" + path;
        }
        return path;
    }

    function normalizedPageMode(mode) {
        return mode === "single" || mode === "two" ? mode : root.defaultPageMode;
    }

    function currentPageMode() {
        return root.twoPageMode ? "two" : "single";
    }

    function applyPageMode(mode) {
        const requestedMode = root.normalizedPageMode(mode);
        const nextTwoPageMode = requestedMode === "two" && document.pageCount > 1;
        const activeMode = nextTwoPageMode ? "two" : "single";

        if (root.twoPageMode !== nextTwoPageMode) {
            root.twoPageMode = nextTwoPageMode;
        }

        return activeMode;
    }

    function samePageSize(firstPage, secondPage) {
        if (!document || firstPage < 0 || secondPage < 0 || firstPage >= document.pageCount || secondPage >= document.pageCount) {
            return false;
        }

        const firstSize = document.pagePointSize(firstPage);
        const secondSize = document.pagePointSize(secondPage);
        return Math.abs(firstSize.width - secondSize.width) < 0.01 && Math.abs(firstSize.height - secondSize.height) < 0.01;
    }

    function hasCoverPage() {
        return document.opened && document.pageCount >= 3 && !root.samePageSize(0, 1) && root.samePageSize(1, 2);
    }

    function setPageMode(mode, persist) {
        const activeMode = root.applyPageMode(mode);
        if (root.pageMode !== activeMode) {
            root.pageMode = activeMode;
        }
        if (persist) {
            root.pageModeSaved(activeMode);
        }
    }

    function boundedDocumentPage(page) {
        if (document.pageCount <= 0) {
            return 0;
        }

        const pageNumber = Number(page);
        return Math.max(0, Math.min(isNaN(pageNumber) ? 0 : pageNumber, document.pageCount - 1));
    }

    function pageForDisplay(page) {
        const boundedPage = root.boundedDocumentPage(page);
        if (!root.twoPageMode) {
            return boundedPage;
        }

        if (root.coverPageMode && boundedPage === 0) {
            return 0;
        }

        const segmentStart = root.spreadSegmentStartForPage(boundedPage);
        const firstContentPage = root.coverPageMode ? 1 : 0;
        const effectiveSegmentStart = Math.max(firstContentPage, segmentStart);
        return effectiveSegmentStart + Math.floor((boundedPage - effectiveSegmentStart) / 2) * 2;
    }

    function isSpreadBoundaryPage(page) {
        const boundedPage = root.boundedDocumentPage(page);
        const pages = root.primaryTableOfContentsPages || [];
        for (let index = 0; index < pages.length; ++index) {
            if (Number(pages[index]) === boundedPage) {
                return true;
            }
        }
        return false;
    }

    function secondPageForSpread(firstPage) {
        if (!root.twoPageMode || !document.opened || document.pageCount <= 0) {
            return -1;
        }

        const secondPage = root.boundedDocumentPage(firstPage) + 1;
        if (root.coverPageMode && firstPage === 0) {
            return -1;
        }
        if (secondPage >= document.pageCount || root.isSpreadBoundaryPage(secondPage)) {
            return -1;
        }
        return secondPage;
    }

    function displayedPageText(page) {
        const firstPage = root.pageForDisplay(page);
        const secondPage = root.secondPageForSpread(firstPage);
        if (root.coverPageMode && firstPage === 0) {
            return i18nc("@info:status PDF cover page", "Cover");
        }
        if (secondPage >= 0) {
            return i18nc("@info:status PDF pages shown in two-page mode", "%1/%2", firstPage, secondPage);
        }
        return String(firstPage);
    }

    function pageValueFromText(text) {
        const match = String(text).match(/[0-9]+/);
        if (!match) {
            return Math.max(1, root.activePage + 1);
        }

        const pageNumber = Number(match[0]);
        if (!isFinite(pageNumber)) {
            return Math.max(1, root.activePage + 1);
        }

        const value = root.coverPageMode ? pageNumber + 1 : pageNumber;
        return Math.max(1, Math.min(Math.max(1, document.pageCount), Math.floor(value)));
    }

    function pageForReadingPosition() {
        if (!document.opened || document.pageCount <= 0 || root.activePage < 0) {
            return -1;
        }

        return root.boundedDocumentPage(root.twoPageMode ? view.spreadFirstPage : root.activePage);
    }

    function goToPage(page) {
        view.goToPage(root.pageForDisplay(page));
    }

    function goToPreviousPage() {
        view.goToPreviousPage();
    }

    function goToNextPage() {
        view.goToNextPage();
    }

    function searchBack() {
        view.searchBack();
    }

    function searchForward() {
        view.searchForward();
    }

    function loadTableOfContentsDrawer() {
        const drawer = applicationWindow().contextDrawer;
        drawer.tableOfContentsModified = (root.manualTableOfContentsEntries || []).length > 0;
        drawer.clearCurrentItem();
        const tableOfContentsJson = root.tableOfContentsJsonWithManualEntries();
        drawer.model.importFromJson(tableOfContentsJson);
        root.primaryTableOfContentsEntries = root.primaryEntriesFromTableOfContents(tableOfContentsJson);
        root.primaryTableOfContentsPages = root.primaryTableOfContentsEntries.map(entry => entry.page);
        root.tableOfContentsEntries = root.flattenTableOfContents(tableOfContentsJson);
        root.updateCurrentTableOfContentsItem(root.readingPage);
    }

    function tableOfContentsJsonWithManualEntries() {
        let entries = [];
        try {
            entries = JSON.parse(document.tableOfContentsJson || "[]");
        } catch (error) {}

        return JSON.stringify(entries.concat(root.manualTableOfContentsEntries || []));
    }

    function updateCurrentTableOfContentsItem(page) {
        const drawer = applicationWindow().contextDrawer;
        if (!drawer) {
            return;
        }

        if (!document.opened || !isFinite(page) || page < 0) {
            drawer.clearCurrentItem();
            return;
        }

        const currentPage = root.boundedDocumentPage(page);
        const entries = root.tableOfContentsEntries || [];
        let currentEntry = null;
        for (let index = 0; index < entries.length; ++index) {
            if (entries[index].page <= currentPage) {
                currentEntry = entries[index];
            } else {
                break;
            }
        }

        drawer.currentTocId = currentEntry && currentEntry.id ? String(currentEntry.id) : "";
        drawer.currentHref = currentEntry && currentEntry.href ? String(currentEntry.href) : "";
    }

    function prepareManualTableOfContentsEntry(page) {
        manualTableOfContentsPage = root.boundedDocumentPage(page);
        manualTableOfContentsTitle.text = i18nc("@label", "Page %1", manualTableOfContentsPage + 1);
        manualTableOfContentsDialog.open();
        manualTableOfContentsTitle.forceActiveFocus();
        manualTableOfContentsTitle.selectAll();
    }

    function addManualTableOfContentsEntry() {
        if (!document.opened || manualTableOfContentsPage < 0) {
            return;
        }

        const title = manualTableOfContentsTitle.text.trim();
        if (title.length === 0) {
            return;
        }

        const entries = (root.manualTableOfContentsEntries || []).slice();
        const page = root.boundedDocumentPage(manualTableOfContentsPage);
        const entry = {
            label: title,
            id: "pdf-manual-toc-" + page,
            href: root.locationForPage(page),
            subitems: []
        };
        const existingIndex = entries.findIndex(item => root.pageFromLocation(item.href) === page);
        if (existingIndex >= 0) {
            entries[existingIndex] = entry;
        } else {
            entries.push(entry);
        }
        root.manualTableOfContentsEntries = entries;
        root.loadTableOfContentsDrawer();
    }

    function saveTableOfContents() {
        const result = document.writeTableOfContentsToPdf(JSON.stringify(root.manualTableOfContentsEntries || []), root.filename);
        if (!result || !result.success) {
            console.warn(result && result.message ? result.message : "Unable to write PDF table of contents.");
            return;
        }

        root.manualTableOfContentsEntries = [];
        root.loadTableOfContentsDrawer();
        document.reload();
    }

    function primaryEntriesFromTableOfContents(tableOfContentsJson) {
        const result = [];
        const pageMap = {};

        try {
            const entries = typeof tableOfContentsJson === "string" ? JSON.parse(tableOfContentsJson) : tableOfContentsJson;
            if (!Array.isArray(entries)) {
                return [];
            }

            for (let index = 0; index < entries.length; ++index) {
                const href = entries[index] && entries[index].href ? entries[index].href : "";
                if (href.length === 0) {
                    continue;
                }

                const page = root.pageFromLocation(href);
                if (isFinite(page) && page >= 0) {
                    const boundedPage = root.boundedDocumentPage(page);
                    const pageKey = String(boundedPage);
                    if (!pageMap[pageKey]) {
                        pageMap[pageKey] = true;
                        result.push({
                            id: entries[index].id || "",
                            page: boundedPage,
                            href: href,
                            title: entries[index].label || ""
                        });
                    }
                }
            }
        } catch (error) {}

        result.sort((first, second) => first.page - second.page);
        return result;
    }

    function spreadSegmentStartForPage(page) {
        const boundedPage = root.boundedDocumentPage(page);
        const pages = root.primaryTableOfContentsPages || [];
        let segmentStart = 0;
        for (let index = 0; index < pages.length; ++index) {
            const primaryPage = pages[index];
            if (primaryPage > boundedPage) {
                break;
            }
            segmentStart = Math.max(segmentStart, primaryPage);
        }
        return segmentStart;
    }

    function goToTableOfContentsLocation(location) {
        if (!location) {
            return;
        }

        root.goToPdfLocation(location);
    }

    function goToPdfLocation(location) {
        if (!location) {
            return;
        }

        const normalizedLocation = root.normalizedPdfLocation(location);
        if (!normalizedLocation) {
            return;
        }

        const page = root.pageFromLocation(normalizedLocation);
        const parsed = root.pdfLocationObject(normalizedLocation);
        const x = parsed && parsed.x !== undefined ? Number(parsed.x) : NaN;
        const y = parsed && parsed.y !== undefined ? Number(parsed.y) : NaN;
        if (isFinite(x) && isFinite(y)) {
            view.goToPagePosition(page, x, y, parsed.position || "");
        } else {
            root.goToPage(page);
        }
        root.updateReadingPosition();
        root.pdfHistoryCurrentLocation = normalizedLocation;
    }

    function setPdfHistory(backHistory, forwardHistory) {
        root.pdfBackHistory = backHistory;
        root.pdfForwardHistory = forwardHistory;
        root.pdfCanGoBack = root.pdfBackHistory.length > 0;
        root.pdfCanGoForward = root.pdfForwardHistory.length > 0;
    }

    function clearPdfHistory() {
        root.setPdfHistory([], []);
        root.pdfHistoryCurrentLocation = "";
    }

    function pushPdfHistoryEntry(stack, location) {
        const normalizedLocation = root.normalizedPdfLocation(location);
        if (!normalizedLocation) {
            return stack.slice();
        }

        const result = stack.slice();
        if (result.length === 0 || result[result.length - 1] !== normalizedLocation) {
            result.push(normalizedLocation);
        }
        while (result.length > root.maximumPdfHistoryEntries) {
            result.shift();
        }
        return result;
    }

    function currentPdfHistoryLocation() {
        const historyLocation = root.normalizedPdfLocation(root.pdfHistoryCurrentLocation);
        if (historyLocation) {
            return historyLocation;
        }

        const page = root.readingPage >= 0 ? root.readingPage : root.activePage;
        return root.normalizedPdfLocation(root.locationForPage(page));
    }

    function followPdfLink(location, sourceLocation) {
        const targetLocation = root.normalizedPdfLocation(location);
        if (!targetLocation) {
            return;
        }

        const previousLocation = root.normalizedPdfLocation(sourceLocation) || root.currentPdfHistoryLocation();
        const nextBackHistory = previousLocation && previousLocation !== targetLocation ? root.pushPdfHistoryEntry(root.pdfBackHistory, previousLocation) : root.pdfBackHistory.slice();
        root.setPdfHistory(nextBackHistory, []);
        root.goToPdfLocation(targetLocation);
    }

    function goBack() {
        if (!root.pdfCanGoBack) {
            return;
        }

        const backHistory = root.pdfBackHistory.slice();
        const targetLocation = backHistory.pop();
        const currentLocation = root.currentPdfHistoryLocation();
        const forwardHistory = currentLocation && currentLocation !== targetLocation ? root.pushPdfHistoryEntry(root.pdfForwardHistory, currentLocation) : root.pdfForwardHistory.slice();

        root.setPdfHistory(backHistory, forwardHistory);
        root.goToPdfLocation(targetLocation);
    }

    function goForward() {
        if (!root.pdfCanGoForward) {
            return;
        }

        const forwardHistory = root.pdfForwardHistory.slice();
        const targetLocation = forwardHistory.pop();
        const currentLocation = root.currentPdfHistoryLocation();
        const backHistory = currentLocation && currentLocation !== targetLocation ? root.pushPdfHistoryEntry(root.pdfBackHistory, currentLocation) : root.pdfBackHistory.slice();

        root.setPdfHistory(backHistory, forwardHistory);
        root.goToPdfLocation(targetLocation);
    }

    function flattenTableOfContents(tableOfContentsJson) {
        const result = [];
        let order = 0;

        function appendEntries(entries, depth) {
            if (!Array.isArray(entries)) {
                return;
            }

            for (let index = 0; index < entries.length; ++index) {
                const entry = entries[index] || {};
                const href = entry.href || "";
                if (href.length > 0) {
                    const page = root.pageFromLocation(href);
                    if (isFinite(page) && page >= 0) {
                        result.push({
                            id: entry.id || "",
                            page: root.boundedDocumentPage(page),
                            href: href,
                            title: entry.label || "",
                            depth: depth,
                            order: order++
                        });
                    }
                }

                appendEntries(entry.subitems || [], depth + 1);
            }
        }

        try {
            appendEntries(typeof tableOfContentsJson === "string" ? JSON.parse(tableOfContentsJson) : tableOfContentsJson, 0);
        } catch (error) {}

        result.sort((first, second) => {
            if (first.page !== second.page) {
                return first.page - second.page;
            }
            if (first.depth !== second.depth) {
                return first.depth - second.depth;
            }
            return first.order - second.order;
        });

        return result;
    }

    function goToSection(previous) {
        const primaryEntries = root.primaryTableOfContentsEntries || [];
        const entries = primaryEntries.length > 0 ? primaryEntries : (root.tableOfContentsEntries || []);
        if (entries.length === 0 || document.pageCount <= 0) {
            return;
        }

        const currentPage = root.readingPage >= 0 ? root.readingPage : root.activePage;
        const currentDisplayPage = root.pageForDisplay(currentPage);
        let targetEntry = null;
        if (previous) {
            for (let index = entries.length - 1; index >= 0; --index) {
                const entryDisplayPage = root.pageForDisplay(entries[index].page);
                if (entryDisplayPage < currentDisplayPage) {
                    targetEntry = entries[index];
                    break;
                }
            }
        } else {
            for (let index = 0; index < entries.length; ++index) {
                const entryDisplayPage = root.pageForDisplay(entries[index].page);
                if (entryDisplayPage > currentDisplayPage) {
                    targetEntry = entries[index];
                    break;
                }
            }
        }

        if (!targetEntry) {
            return;
        }

        root.goToPage(targetEntry.page);
        root.updateReadingPosition();
    }

    function goToPreviousSection() {
        root.goToSection(true);
    }

    function goToNextSection() {
        root.goToSection(false);
    }

    function locationForPage(page) {
        const normalizedPage = Math.max(0, page);
        return JSON.stringify({
            format: "pdf",
            page: normalizedPage,
            pageNumber: normalizedPage + 1
        });
    }

    function pageFromLocation(location) {
        if (!location) {
            return 0;
        }

        if (typeof location === "object") {
            if (location.format === "pdf" && location.page !== undefined) {
                const page = Number(location.page);
                return isNaN(page) ? 0 : Math.max(0, page);
            }
            if (location.format === "pdf" && location.pageNumber !== undefined) {
                const pageNumber = Number(location.pageNumber);
                return isNaN(pageNumber) ? 0 : Math.max(0, pageNumber - 1);
            }
        }

        try {
            const parsed = JSON.parse(location);
            if (parsed && parsed.format === "pdf" && parsed.page !== undefined) {
                const page = Number(parsed.page);
                return isNaN(page) ? 0 : Math.max(0, page);
            }
            if (parsed && parsed.format === "pdf" && parsed.pageNumber !== undefined) {
                const pageNumber = Number(parsed.pageNumber);
                return isNaN(pageNumber) ? 0 : Math.max(0, pageNumber - 1);
            }
        } catch (error) {}

        const page = Number(location);
        return isNaN(page) ? 0 : Math.max(0, page);
    }

    function pdfLocationObject(location) {
        if (!location) {
            return null;
        }

        if (typeof location === "object") {
            return location.format === "pdf" ? location : null;
        }

        try {
            const parsed = JSON.parse(location);
            return parsed && parsed.format === "pdf" ? parsed : null;
        } catch (error) {}

        return null;
    }

    function normalizedPdfLocation(location) {
        if (!location) {
            return "";
        }

        const page = root.boundedDocumentPage(root.pageFromLocation(location));
        const result = {
            format: "pdf",
            page: page,
            pageNumber: page + 1
        };
        const parsed = root.pdfLocationObject(location);
        if (parsed) {
            const x = Number(parsed.x);
            const y = Number(parsed.y);
            if (isFinite(x) && isFinite(y)) {
                result.x = Math.max(0, Math.min(1, x));
                result.y = Math.max(0, Math.min(1, y));
                result.position = parsed.position === "topLeft" ? "topLeft" : "center";
            }
        }

        return JSON.stringify(result);
    }

    function normalizedPdfColorRegion(region) {
        if (!region) {
            return null;
        }

        const x = Number(region.x);
        const y = Number(region.y);
        const width = Number(region.width);
        const height = Number(region.height);
        if (!isFinite(x) || !isFinite(y) || !isFinite(width) || !isFinite(height)) {
            return null;
        }

        const left = Math.max(0, Math.min(1, Math.min(x, x + width)));
        const top = Math.max(0, Math.min(1, Math.min(y, y + height)));
        const right = Math.max(0, Math.min(1, Math.max(x, x + width)));
        const bottom = Math.max(0, Math.min(1, Math.max(y, y + height)));
        if (right - left < 0.002 || bottom - top < 0.002) {
            return null;
        }

        const normalizedRegion = {
            x: left,
            y: top,
            width: right - left,
            height: bottom - top
        };
        if (region.shape === "ellipse") {
            normalizedRegion.shape = "ellipse";
        }
        return normalizedRegion;
    }

    function normalizedRegionPageMap(pages) {
        const result = {};
        if (!pages || typeof pages !== "object") {
            return result;
        }

        const pageKeys = Object.keys(pages);
        for (let i = 0; i < pageKeys.length; ++i) {
            const pageNumber = Number(pageKeys[i]);
            if (!isFinite(pageNumber) || pageNumber < 0) {
                continue;
            }

            const regions = pages[pageKeys[i]];
            if (!Array.isArray(regions)) {
                continue;
            }

            const normalizedRegions = [];
            for (let regionIndex = 0; regionIndex < regions.length; ++regionIndex) {
                const normalizedRegion = root.normalizedPdfColorRegion(regions[regionIndex]);
                if (normalizedRegion) {
                    normalizedRegions.push(normalizedRegion);
                }
            }
            if (normalizedRegions.length > 0) {
                result[String(Math.floor(pageNumber))] = normalizedRegions;
            }
        }

        return result;
    }

    function parseNotInvertedRegionData(value) {
        const result = {
            version: 3,
            pages: {},
            forcedInvertedPages: {}
        };
        if (!value) {
            return result;
        }

        try {
            const parsed = typeof value === "string" ? JSON.parse(value) : value;
            const pages = parsed && parsed.pages && typeof parsed.pages === "object" ? parsed.pages : {};
            const forcedInvertedPages = parsed && parsed.forcedInvertedPages && typeof parsed.forcedInvertedPages === "object" ? parsed.forcedInvertedPages : {};
            result.pages = root.normalizedRegionPageMap(pages);
            result.forcedInvertedPages = root.normalizedRegionPageMap(forcedInvertedPages);
        } catch (error) {}

        return result;
    }

    function cloneNotInvertedRegionData() {
        return root.parseNotInvertedRegionData(root.pdfNotInvertedRegions);
    }

    function serializedNotInvertedRegionData(data) {
        const pages = root.normalizedRegionPageMap(data && data.pages ? data.pages : {});
        const forcedInvertedPages = root.normalizedRegionPageMap(data && data.forcedInvertedPages ? data.forcedInvertedPages : {});

        if (Object.keys(pages).length === 0 && Object.keys(forcedInvertedPages).length === 0) {
            return "";
        }

        return JSON.stringify({
            version: 3,
            pages: pages,
            forcedInvertedPages: forcedInvertedPages
        });
    }

    function saveNotInvertedRegionData(data) {
        const serializedRegions = root.serializedNotInvertedRegionData(data);
        root.pdfNotInvertedRegions = serializedRegions;
        root.pdfNotInvertedRegionsSaved(serializedRegions);
    }

    function notInvertedRegionsForPage(page) {
        root.pdfNotInvertedRegionsVersion;
        const pageNumber = Math.max(0, Math.floor(Number(page)));
        if (!isFinite(pageNumber)) {
            return [];
        }

        const data = root.pdfNotInvertedRegionData || {
            pages: {}
        };
        const storedRegions = data.pages && data.pages[String(pageNumber)] ? data.pages[String(pageNumber)] : [];
        const regions = storedRegions.slice();
        if (root.pendingNotInvertedRegion && root.pendingNotInvertedRegion.page === pageNumber) {
            regions.push(root.pendingNotInvertedRegion.rect);
        }
        return regions;
    }

    function notInvertedRegionCountForPage(page) {
        return root.notInvertedRegionsForPage(page).length;
    }

    function forcedInvertedRegionsForPage(page) {
        root.pdfNotInvertedRegionsVersion;
        const pageNumber = Math.max(0, Math.floor(Number(page)));
        if (!isFinite(pageNumber)) {
            return [];
        }

        const data = root.pdfNotInvertedRegionData || {
            forcedInvertedPages: {}
        };
        const storedRegions = data.forcedInvertedPages && data.forcedInvertedPages[String(pageNumber)] ? data.forcedInvertedPages[String(pageNumber)] : [];
        const regions = storedRegions.slice();
        if (root.pendingForcedInvertedRegion && root.pendingForcedInvertedRegion.page === pageNumber) {
            regions.push(root.pendingForcedInvertedRegion.rect);
        }
        return regions;
    }

    function forcedInvertedRegionCountForPage(page) {
        return root.forcedInvertedRegionsForPage(page).length;
    }

    function pdfRegionCoversWholePage(region) {
        const normalizedRegion = root.normalizedPdfColorRegion(region);
        return normalizedRegion && normalizedRegion.shape !== "ellipse" && normalizedRegion.x <= 0.002 && normalizedRegion.y <= 0.002 && normalizedRegion.x + normalizedRegion.width >= 0.998 && normalizedRegion.y + normalizedRegion.height >= 0.998;
    }

    function preserveColorForPage(page) {
        if (!document.opened) {
            return;
        }

        const pageNumber = root.boundedDocumentPage(page);
        const pageKey = String(pageNumber);
        const data = root.cloneNotInvertedRegionData();
        const regions = data.pages[pageKey] ? data.pages[pageKey].slice() : [];
        if (!regions.some(region => root.pdfRegionCoversWholePage(region))) {
            regions.push({
                x: 0,
                y: 0,
                width: 1,
                height: 1
            });
        }

        data.pages[pageKey] = regions;
        root.pendingNotInvertedRegion = null;
        root.pendingForcedInvertedRegion = null;
        root.saveNotInvertedRegionData(data);
        root.notInvertedRegionSelectionMode = false;
        root.ovalNotInvertedRegionSelectionMode = false;
        root.forcedInvertedRegionSelectionMode = false;
    }

    function chooseRectangleArea() {
        if (!document.opened) {
            return;
        }

        root.forcedInvertedRegionSelectionMode = false;
        root.ovalNotInvertedRegionSelectionMode = false;
        root.notInvertedRegionSelectionMode = true;
        view.clearSelection();
    }

    function chooseOvalArea() {
        if (!document.opened) {
            return;
        }

        root.forcedInvertedRegionSelectionMode = false;
        root.notInvertedRegionSelectionMode = false;
        root.ovalNotInvertedRegionSelectionMode = true;
        view.clearSelection();
    }

    function chooseInvertRectangleArea() {
        if (!document.opened) {
            return;
        }

        root.notInvertedRegionSelectionMode = false;
        root.ovalNotInvertedRegionSelectionMode = false;
        root.forcedInvertedRegionSelectionMode = true;
        view.clearSelection();
    }

    function requestNotInvertedRegion(page, region) {
        const normalizedRegion = root.normalizedPdfColorRegion(region);
        if (!normalizedRegion) {
            return;
        }

        root.pendingNotInvertedRegion = {
            page: root.boundedDocumentPage(page),
            rect: normalizedRegion
        };
        root.pdfNotInvertedRegionsVersion += 1;
        notInvertedRegionDialog.open();
    }

    function requestForcedInvertedRegion(page, region) {
        const normalizedRegion = root.normalizedPdfColorRegion(region);
        if (!normalizedRegion) {
            return;
        }

        root.pendingForcedInvertedRegion = {
            page: root.boundedDocumentPage(page),
            rect: normalizedRegion
        };
        root.pdfNotInvertedRegionsVersion += 1;
        forcedInvertedRegionDialog.open();
    }

    function confirmPendingNotInvertedRegion() {
        if (!root.pendingNotInvertedRegion) {
            return;
        }

        const data = root.cloneNotInvertedRegionData();
        const pageKey = String(root.pendingNotInvertedRegion.page);
        const regions = data.pages[pageKey] ? data.pages[pageKey].slice() : [];
        regions.push(root.pendingNotInvertedRegion.rect);
        data.pages[pageKey] = regions;
        root.pendingNotInvertedRegion = null;
        root.saveNotInvertedRegionData(data);
        root.notInvertedRegionSelectionMode = false;
        root.ovalNotInvertedRegionSelectionMode = false;
    }

    function confirmPendingForcedInvertedRegion() {
        if (!root.pendingForcedInvertedRegion) {
            return;
        }

        const data = root.cloneNotInvertedRegionData();
        const pageKey = String(root.pendingForcedInvertedRegion.page);
        const regions = data.forcedInvertedPages[pageKey] ? data.forcedInvertedPages[pageKey].slice() : [];
        regions.push(root.pendingForcedInvertedRegion.rect);
        data.forcedInvertedPages[pageKey] = regions;
        root.pendingForcedInvertedRegion = null;
        root.saveNotInvertedRegionData(data);
        root.forcedInvertedRegionSelectionMode = false;
    }

    function discardPendingNotInvertedRegion() {
        if (!root.pendingNotInvertedRegion) {
            return;
        }

        root.pendingNotInvertedRegion = null;
        root.pdfNotInvertedRegionsVersion += 1;
        root.notInvertedRegionSelectionMode = false;
        root.ovalNotInvertedRegionSelectionMode = false;
    }

    function discardPendingForcedInvertedRegion() {
        if (!root.pendingForcedInvertedRegion) {
            return;
        }

        root.pendingForcedInvertedRegion = null;
        root.pdfNotInvertedRegionsVersion += 1;
        root.forcedInvertedRegionSelectionMode = false;
    }

    function clearNotInvertedRegionsForPage(page) {
        const data = root.cloneNotInvertedRegionData();
        const pageKey = String(root.boundedDocumentPage(page));
        if (!data.pages[pageKey]) {
            return;
        }

        delete data.pages[pageKey];
        root.saveNotInvertedRegionData(data);
    }

    function clearForcedInvertedRegionsForPage(page) {
        const data = root.cloneNotInvertedRegionData();
        const pageKey = String(root.boundedDocumentPage(page));
        if (!data.forcedInvertedPages[pageKey]) {
            return;
        }

        delete data.forcedInvertedPages[pageKey];
        root.saveNotInvertedRegionData(data);
    }

    function progressForPage(page) {
        if (document.pageCount <= 0 || page < 0) {
            return 0;
        }

        return Math.round(((Math.max(0, page) + 1) / document.pageCount) * 100);
    }

    function updateReadingPosition() {
        if (root.restoringInitialLocation) {
            return;
        }

        const page = root.pageForReadingPosition();
        if (page < 0) {
            return;
        }

        root.currentLocation = root.locationForPage(page);
        root.pdfHistoryCurrentLocation = root.currentLocation;
        root.updateCurrentTableOfContentsItem(page);
        root.relocated(root.currentLocation, root.progressForPage(page));
    }

    function applyInitialLocation() {
        if (root.initialLocationApplied || !document.opened || document.pageCount <= 0) {
            return;
        }

        root.initialLocationApplied = true;
        const initialPage = root.pageFromLocation(root.currentLocation);
        root.restoringInitialLocation = true;
        root.applyPageMode(root.effectivePageMode);
        const page = root.pageForDisplay(initialPage);
        root.goToPage(page);
        root.restoringInitialLocation = false;
        root.updateReadingPosition();
    }

    function metadataList(value) {
        if (!value) {
            return [];
        }
        if (Array.isArray(value)) {
            return value;
        }
        if (typeof value !== "string" && typeof value.length === "number") {
            const result = [];
            for (let i = 0; i < value.length; ++i) {
                result.push(value[i]);
            }
            return result;
        }
        return [String(value)];
    }

    function detailsMetadata() {
        const entry = root.entry || {};
        const filename = entry.filename || root.filename || "";
        const filetitle = entry.filetitle || root.filetitle;
        const authors = root.metadataList(entry.author).length > 0 ? root.metadataList(entry.author) : root.metadataList(document.author);
        const subjects = root.metadataList(entry.genres).length > 0 ? root.metadataList(entry.genres) : root.metadataList(document.subject);
        const keywords = root.metadataList(entry.keywords).length > 0 ? root.metadataList(entry.keywords) : root.metadataList(document.keywords);

        return {
            filename: filename,
            filetitle: filetitle,
            title: document.title || entry.title || filetitle,
            author: authors,
            description: document.subject || "",
            publisher: entry.publisher || document.producer || "",
            language: entry.language || "",
            pubdate: entry.created || document.creationDate || "",
            rights: entry.rights || "",
            thumbnail: entry.thumbnail || "",
            passwordProtected: Boolean(entry.passwordProtected || document.needsPassword),
            identifier: entry.identifier || "",
            uniqueIdentifier: entry.uniqueIdentifier || "",
            source: entry.source || document.creator || "",
            genres: subjects.length > 0 ? subjects : keywords,
            zoomLevel: root.zoomLevel,
            pageMode: root.currentPageMode()
        };
    }

    function importReadOnlyBook(callback) {
        if (!root.readOnly) {
            if (typeof callback === "function") {
                callback(root.entry);
            }
            return;
        }

        root.addToLibraryRequested(callback);
    }

    function refreshLibraryEntryFromFile() {
        const refreshedEntry = applicationWindow().bookListModel.refreshBookFromFile(root.filename, true);
        if (!root.readOnly && refreshedEntry.filename && refreshedEntry.filename.length > 0) {
            root.entry = refreshedEntry;
            root.applyUpdatedBookEntry(refreshedEntry);
        }
    }

    function reloadCurrentBook() {
        if (pdfIoLoader.loading || document.loading) {
            reloadCurrentBookTimer.restart();
            return;
        }

        root.updateReadingPosition();
        root.initialLocationApplied = false;
        pdfIoLoader.reload();
    }

    function pdfEditorCommand() {
        return (Config.pdfEditorCommand || "").trim();
    }

    function canEditBook() {
        return root.pdfEditorCommand().length > 0 && root.filename.length > 0 && root.documentReady;
    }

    function currentEditorPageNumber() {
        const maximumPage = Math.max(1, document.pageCount);
        const currentPage = root.activePage >= 0 ? root.activePage + 1 : 1;
        return Math.max(1, Math.min(currentPage, maximumPage));
    }

    function editBook() {
        if (!root.canEditBook()) {
            return;
        }

        root.updateReadingPosition();
        if (!ExternalProcess.startDetached(root.pdfEditorCommand(), ["--page", String(root.currentEditorPageNumber()), root.filename])) {
            console.warn("Unable to start PDF editor for:", root.filename);
        }
    }

    function showTranslation(text) {
        translatedText.text = text;
        translatorDialog.open();
    }

    function translateSelection(selectedText) {
        const text = String(selectedText || "").trim();
        if (text.length === 0) {
            return;
        }

        root.translationPending = true;
        root.showTranslation(i18n("Translating..."));
        Translator.translate(text);
    }

    function applyUpdatedBookEntry(updatedEntry) {
        if (!updatedEntry || !updatedEntry.filename || updatedEntry.filename !== root.filename) {
            return;
        }

        root.entry = updatedEntry;
        if (updatedEntry.pdfNotInvertedRegions !== undefined) {
            root.pdfNotInvertedRegions = updatedEntry.pdfNotInvertedRegions || "";
        }
        if (updatedEntry.currentLocation) {
            root.currentLocation = updatedEntry.currentLocation;
        }
        if (updatedEntry.zoomLevel > 0) {
            root.setZoom(updatedEntry.zoomLevel);
        }
        if (updatedEntry.pageMode) {
            root.pageMode = root.normalizedPageMode(updatedEntry.pageMode);
            root.applyPageMode(root.pageMode);
        }
    }

    onUrlChanged: {
        root.initialLocationApplied = false;
        root.restoringInitialLocation = false;
        root.manualTableOfContentsEntries = [];
        applicationWindow().contextDrawer.tableOfContentsModified = false;
        root.clearPdfHistory();
    }

    onTwoPageModeChanged: {
        root.goToPage(root.activePage);
        root.updateReadingPosition();
    }

    onPdfNotInvertedRegionsChanged: {
        root.pdfNotInvertedRegionData = root.parseNotInvertedRegionData(root.pdfNotInvertedRegions);
        root.pdfNotInvertedRegionsVersion += 1;
    }

    onNotInvertedRegionSelectionModeChanged: {
        if (root.notInvertedRegionSelectionMode) {
            root.ovalNotInvertedRegionSelectionMode = false;
            root.forcedInvertedRegionSelectionMode = false;
            view.clearSelection();
        }
    }

    onOvalNotInvertedRegionSelectionModeChanged: {
        if (root.ovalNotInvertedRegionSelectionMode) {
            root.notInvertedRegionSelectionMode = false;
            root.forcedInvertedRegionSelectionMode = false;
            view.clearSelection();
        }
    }

    onForcedInvertedRegionSelectionModeChanged: {
        if (root.forcedInvertedRegionSelectionMode) {
            root.notInvertedRegionSelectionMode = false;
            root.ovalNotInvertedRegionSelectionMode = false;
            view.clearSelection();
        }
    }

    Component.onCompleted: {
        root.pdfNotInvertedRegionData = root.parseNotInvertedRegionData(root.pdfNotInvertedRegions);
    }

    Component.onDestruction: {
        zoomSaveTimer.flush();
        root.updateReadingPosition();
        applicationWindow().contextDrawer.tableOfContentsModified = false;
        root.bookClosed();
    }

    QQC2.ActionGroup {
        id: readerThemeActionGroup
        exclusive: true
    }

    actions: [
        Kirigami.Action {
            text: i18nc("@action:intoolbar", "Search")
            displayHint: Kirigami.DisplayHint.IconOnly
            icon.name: "system-search-symbolic"
            onTriggered: {
                root.searchVisible = !root.searchVisible;
                if (root.searchVisible) {
                    searchField.forceActiveFocus();
                }
            }
        },
        Kirigami.Action {
            text: root.twoPageMode ? i18nc("@action:intoolbar", "Two-Page View") : i18nc("@action:intoolbar", "One-Page View")
            displayHint: Kirigami.DisplayHint.IconOnly
            icon.name: root.twoPageMode ? "view-pages-facing" : "view-pages-single"
            checkable: true
            checked: root.twoPageMode
            enabled: document.opened && document.pageCount > 1
            onTriggered: root.setPageMode(root.twoPageMode ? "single" : "two", true)
        },
        Kirigami.Action {
            text: i18nc("@action:intoolbar", "Reader theme")
            displayHint: Kirigami.DisplayHint.IconOnly
            icon.name: "preferences-desktop-theme"

            Kirigami.Action {
                text: i18nc("@action:inmenu Reader color theme", "Normal")
                checkable: true
                checked: root.currentReaderTheme === 0
                QQC2.ActionGroup.group: readerThemeActionGroup
                onTriggered: root.setReaderThemeMode(0)
            }

            Kirigami.Action {
                text: i18nc("@action:inmenu Reader color theme", "Inverted")
                checkable: true
                checked: root.currentReaderTheme === 1
                QQC2.ActionGroup.group: readerThemeActionGroup
                onTriggered: root.setReaderThemeMode(1)
            }

            Kirigami.Action {
                text: i18nc("@action:inmenu Reader color theme", "System")
                checkable: true
                checked: root.currentReaderTheme === 2
                QQC2.ActionGroup.group: readerThemeActionGroup
                onTriggered: root.setReaderThemeMode(2)
            }
        },
        Kirigami.Action {
            text: i18nc("@action:intoolbar", "Edit Book")
            displayHint: Kirigami.DisplayHint.IconOnly
            icon.name: "document-edit"
            enabled: root.canEditBook()
            onTriggered: root.editBook()
        },
        Kirigami.Action {
            text: i18nc("@action:intoolbar", "Add to Library")
            displayHint: Kirigami.DisplayHint.IconOnly
            icon.name: "list-add"
            visible: root.readOnly
            enabled: root.filename !== ""
            onTriggered: root.importReadOnlyBook()
        },
        Kirigami.Action {
            text: i18n("Book Details")
            displayHint: Kirigami.DisplayHint.IconOnly
            icon.name: "documentinfo"
            enabled: document.opened
            onTriggered: {
                applicationWindow().pageStack.pushDialogLayer(Qt.resolvedUrl("./BookDetailsPage.qml"), {
                    metadata: root.detailsMetadata(),
                    bookListModel: applicationWindow().bookListModel,
                    readOnly: root.readOnly,
                    importToLibrary: function (done) {
                        root.importReadOnlyBook(done);
                    }
                });
            }
        }
    ]

    Connections {
        target: applicationWindow().bookListModel

        function onEntryDataUpdated(entry) {
            root.applyUpdatedBookEntry(entry);
        }
    }

    Connections {
        target: Config

        function onReaderThemeChanged() {
            root.currentReaderTheme = Config.readerTheme;
        }
    }

    Connections {
        target: applicationWindow().contextDrawer

        function onGoTo(location) {
            root.goToTableOfContentsLocation(location);
        }

        function onSaveTableOfContents() {
            root.saveTableOfContents();
        }
    }

    Connections {
        target: Translator

        function onTranslationReady(translatedString) {
            if (!root.translationPending) {
                return;
            }

            root.translationPending = false;
            root.showTranslation(translatedString);
        }
    }

    OkularPdfDocument {
        id: document

        onStatusChanged: {
            if (document.status === OkularPdfDocument.Ready) {
                root.loadTableOfContentsDrawer();
                root.applyInitialLocation();
                root.bookReady(root.displayTitle);
            } else {
                root.loadTableOfContentsDrawer();
            }
        }

        onPageCountChanged: root.applyInitialLocation()
        onTableOfContentsChanged: root.loadTableOfContentsDrawer()

        onPasswordRequired: passwordDialog.open()
    }

    PdfIoDocument {
        id: pdfIoLoader

        target: document
        source: root.url
        watermarkFilterEnabled: applicationWindow().pdfWatermarkFilterMode() === "temporary" && !String(root.url).startsWith("http")
        watermarkFilterPattern: Config.pdfWatermarkFilterPattern || Config.defaultPdfWatermarkFilterPatternValue
    }

    OkularPdfView {
        id: view

        anchors.fill: parent
        document: document
        renderScale: root.clampedScale(root.zoomLevel)
        twoPageMode: root.twoPageMode
        coverPageMode: root.coverPageMode
        inverted: root.pdfPageInverted
        recolor: root.pdfPageUsesSystemColors
        foregroundColor: Kirigami.Theme.textColor
        backgroundColor: Kirigami.Theme.backgroundColor
        readerBackgroundImageSource: root.readerBackgroundImageSource()
        searchString: searchField.text
        zoomStep: root.scaleStep
        primaryTableOfContentsPages: root.primaryTableOfContentsPages
        notInvertedRegionSelectionMode: root.notInvertedRegionSelectionMode
        ovalNotInvertedRegionSelectionMode: root.ovalNotInvertedRegionSelectionMode
        forcedInvertedRegionSelectionMode: root.forcedInvertedRegionSelectionMode
        notInvertedRegionsVersion: root.pdfNotInvertedRegionsVersion
        notInvertedRegionsForPage: page => root.notInvertedRegionsForPage(page)
        forcedInvertedRegionsForPage: page => root.forcedInvertedRegionsForPage(page)

        onCurrentPageChanged: root.updateReadingPosition()
        onZoomRequested: factor => root.adjustZoom(factor)
        onHistoryNavigationRequested: back => {
            if (back) {
                root.goBack();
            } else {
                root.goForward();
            }
        }
        onLinkActivated: (location, sourceLocation) => root.followPdfLink(location, sourceLocation)
        onExternalLinkActivated: (url, sourceLocation) => Qt.openUrlExternally(url)
        onTranslateSelectionRequested: selectedText => root.translateSelection(selectedText)
        onNotInvertedRegionSelected: (page, rect) => root.requestNotInvertedRegion(page, rect)
        onForcedInvertedRegionSelected: (page, rect) => root.requestForcedInvertedRegion(page, rect)
        onPreserveColorForPageRequested: page => root.preserveColorForPage(page)
        onAddPageToTableOfContentsRequested: page => root.prepareManualTableOfContentsEntry(page)
        onChooseRectangleAreaRequested: root.chooseRectangleArea()
        onChooseOvalAreaRequested: root.chooseOvalArea()
        onChooseInvertRectangleAreaRequested: root.chooseInvertRectangleArea()
        onClearPreservedAreasForPageRequested: page => root.clearNotInvertedRegionsForPage(page)
        onClearInvertedAreasForPageRequested: page => root.clearForcedInvertedRegionsForPage(page)
        onSectionNavigationRequested: previous => {
            if (previous) {
                root.goToPreviousSection();
            } else {
                root.goToNextSection();
            }
        }
    }

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        visible: root.url.length === 0
        text: i18n("No PDF selected")
        icon.name: "application-pdf"
    }

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        visible: root.url.length > 0 && (pdfIoLoader.error.length > 0 || document.status === OkularPdfDocument.Error)
        text: pdfIoLoader.error || document.error || i18n("Unable to open PDF")
        icon.name: "dialog-error"
    }

    QQC2.BusyIndicator {
        anchors.centerIn: parent
        running: root.url.length > 0 && (pdfIoLoader.loading || document.status === OkularPdfDocument.Loading)
        visible: running
    }

    Timer {
        id: reloadCurrentBookTimer

        interval: 250
        repeat: false
        onTriggered: root.reloadCurrentBook()
    }

    Window {
        id: translatorDialog

        visible: false
        title: i18nc("@title:window", "Translation")
        transientParent: applicationWindow()
        modality: Qt.NonModal
        flags: Qt.Dialog
        width: Kirigami.Units.gridUnit * 28
        height: Kirigami.Units.gridUnit * 14

        function open() {
            show();
            raise();
        }

        QQC2.Pane {
            anchors.fill: parent

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                QQC2.TextArea {
                    id: translatedText

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    readOnly: true
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    QQC2.Dialog {
        id: passwordDialog

        modal: true
        parent: QQC2.Overlay.overlay
        title: i18n("Password Required")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel

        onAccepted: document.password = passwordField.text
        onRejected: passwordField.clear()

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            QQC2.TextField {
                id: passwordField

                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: i18n("Password")
                onAccepted: passwordDialog.accept()
            }
        }
    }

    QQC2.Dialog {
        id: manualTableOfContentsDialog

        modal: true
        parent: QQC2.Overlay.overlay
        title: i18nc("@title:dialog", "Add Page to Table of Contents")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        onAccepted: root.addManualTableOfContentsEntry()
        onRejected: manualTableOfContentsTitle.clear()

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            QQC2.Label {
                text: i18nc("@info:question", "Enter a title for this PDF page.")
                wrapMode: Text.Wrap
            }

            QQC2.TextField {
                id: manualTableOfContentsTitle

                Layout.fillWidth: true
                onAccepted: manualTableOfContentsDialog.accept()
            }
        }
    }

    QQC2.Dialog {
        id: notInvertedRegionDialog

        modal: true
        parent: QQC2.Overlay.overlay
        title: i18nc("@title:dialog", "Preserve Area Colors?")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        onAccepted: root.confirmPendingNotInvertedRegion()
        onRejected: root.discardPendingNotInvertedRegion()

        contentItem: QQC2.Label {
            width: Math.min(applicationWindow().width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 24)
            text: i18nc("@info:question", "Keep this PDF page area unchanged in inverted and system color themes?")
            wrapMode: Text.Wrap
        }
    }

    QQC2.Dialog {
        id: forcedInvertedRegionDialog

        modal: true
        parent: QQC2.Overlay.overlay
        title: i18nc("@title:dialog", "Invert Area Colors?")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        onAccepted: root.confirmPendingForcedInvertedRegion()
        onRejected: root.discardPendingForcedInvertedRegion()

        contentItem: QQC2.Label {
            width: Math.min(applicationWindow().width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 24)
            text: i18nc("@info:question", "Always transform this PDF page area in inverted and system color themes?")
            wrapMode: Text.Wrap
        }
    }

    Timer {
        id: zoomSaveTimer

        interval: 500
        repeat: false
        onTriggered: root.zoomLevelSaved(root.zoomLevel)

        function flush() {
            if (!running) {
                return;
            }

            stop();
            root.zoomLevelSaved(root.zoomLevel);
        }
    }

    Shortcut {
        sequence: "Ctrl+F"
        onActivated: {
            root.searchVisible = true;
            searchField.forceActiveFocus();
        }
    }

    Shortcut {
        sequence: "Ctrl++"
        onActivated: root.adjustZoom(root.scaleStep)
    }

    Shortcut {
        sequence: "Ctrl+-"
        onActivated: root.adjustZoom(1 / root.scaleStep)
    }

    Shortcut {
        sequence: "Ctrl+R"
        onActivated: root.reloadCurrentBook()
    }

    Shortcut {
        sequence: "Ctrl+T"
        onActivated: root.cycleReaderThemeMode()
    }

    Shortcut {
        sequence: "Alt+Left"
        onActivated: root.goBack()
    }

    Shortcut {
        sequence: "Alt+Right"
        onActivated: root.goForward()
    }

    Shortcut {
        sequence: "Ctrl+Left"
        onActivated: root.goToPreviousSection()
    }

    Shortcut {
        sequence: "Ctrl+Right"
        onActivated: root.goToNextSection()
    }

    Shortcut {
        sequence: "Ctrl+B"
        onActivated: ExternalProcess.startDetached(applicationFilePath)
    }

    Shortcut {
        sequence: "Ctrl+S"
        onActivated: applicationWindow().toggleReaderFullScreen()
    }

    footer: QQC2.ToolBar {
        visible: document.opened

        contentItem: Item {
            implicitHeight: Math.max(searchControls.implicitHeight, pageNavigation.implicitHeight)

            RowLayout {
                id: searchControls

                anchors.left: parent.left
                anchors.right: pageNavigation.left
                anchors.rightMargin: Kirigami.Units.largeSpacing
                anchors.verticalCenter: parent.verticalCenter
                visible: root.searchVisible
                spacing: Kirigami.Units.smallSpacing
                clip: true

                QQC2.TextField {
                    id: searchField

                    Layout.fillWidth: true
                    Layout.minimumWidth: Kirigami.Units.gridUnit * 7
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                    placeholderText: i18nc("@info:placeholder", "Search in PDF...")
                    selectByMouse: true
                }

                QQC2.ToolButton {
                    icon.name: "go-up-search"
                    text: i18nc("@action:intoolbar", "Previous Search Result")
                    display: QQC2.AbstractButton.IconOnly
                    enabled: root.activeSearchResultCount > 0
                    onClicked: root.searchBack()
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }

                QQC2.ToolButton {
                    icon.name: "go-down-search"
                    text: i18nc("@action:intoolbar", "Next Search Result")
                    display: QQC2.AbstractButton.IconOnly
                    enabled: root.activeSearchResultCount > 0
                    onClicked: root.searchForward()
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }

                QQC2.Label {
                    visible: searchField.text.length > 0
                    text: root.activeSearchResultCount > 0 ? i18ncp("@info:status PDF search results", "%1 result", "%1 results", root.activeSearchResultCount) : i18n("No results")
                    elide: Text.ElideRight
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 8
                }
            }

            RowLayout {
                id: pageNavigation

                anchors.centerIn: parent
                spacing: Kirigami.Units.smallSpacing

                QQC2.ToolButton {
                    icon.name: "go-previous-view"
                    text: i18n("Back")
                    display: QQC2.AbstractButton.IconOnly
                    enabled: root.pdfCanGoBack
                    onClicked: root.goBack()
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }

                QQC2.ToolButton {
                    icon.name: "go-previous"
                    text: i18nc("@action:intoolbar", "Previous Page")
                    display: QQC2.AbstractButton.IconOnly
                    enabled: view.hasPreviousPage()
                    onClicked: root.goToPreviousPage()
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }

                QQC2.SpinBox {
                    id: pageSpinBox

                    from: 1
                    to: Math.max(1, document.pageCount)
                    value: Math.max(1, root.activePage + 1)
                    stepSize: root.twoPageMode ? 2 : 1
                    editable: true
                    Layout.preferredWidth: root.twoPageMode ? Kirigami.Units.gridUnit * 6 : Kirigami.Units.gridUnit * 5
                    validator: RegularExpressionValidator {
                        regularExpression: /[0-9]+(\/[0-9]+)?/
                    }
                    textFromValue: function (value, locale) {
                        return root.displayedPageText(value - 1);
                    }
                    valueFromText: function (text, locale) {
                        return root.pageValueFromText(text);
                    }
                    onValueModified: root.goToPage(value - 1)
                }

                QQC2.Label {
                    text: i18nc("@info:status current PDF page count", "of %1", document.pageCount - (root.coverPageMode ? 1 : 0))
                    visible: document.pageCount > 0
                }

                QQC2.ToolButton {
                    icon.name: "go-next"
                    text: i18nc("@action:intoolbar", "Next Page")
                    display: QQC2.AbstractButton.IconOnly
                    enabled: view.hasNextPage()
                    onClicked: root.goToNextPage()
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }

                QQC2.ToolButton {
                    icon.name: "go-next-view"
                    text: i18n("Forward")
                    display: QQC2.AbstractButton.IconOnly
                    enabled: root.pdfCanGoForward
                    onClicked: root.goForward()
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }
            }
        }
    }
}
