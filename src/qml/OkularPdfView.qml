// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.arianna
import org.kde.kirigami as Kirigami

Item {
    id: root

    property OkularPdfDocument document: null
    property int currentPage: 0
    property real renderScale: 1
    property bool twoPageMode: false
    property bool inverted: false
    property bool recolor: false
    property color foregroundColor: "black"
    property color backgroundColor: "white"
    property url readerBackgroundImageSource: ""
    property bool notInvertedRegionSelectionMode: false
    property bool ovalNotInvertedRegionSelectionMode: false
    property bool forcedInvertedRegionSelectionMode: false
    property int notInvertedRegionsVersion: 0
    property var notInvertedRegionsForPage: null
    property var forcedInvertedRegionsForPage: null
    property string searchString: ""
    property real zoomStep: Math.pow(2, 1 / 8)
    property var primaryTableOfContentsPages: []
    property var pendingViewportPosition: null
    property bool coverPageMode: false
    readonly property var searchModel: searchState
    readonly property int spreadFirstPage: twoPageMode ? displayPage(currentPage) : clampPage(currentPage)
    readonly property int spreadSecondPage: twoPageMode ? secondPageForSpread(spreadFirstPage) : -1
    readonly property bool colorRegionSelectionMode: notInvertedRegionSelectionMode || ovalNotInvertedRegionSelectionMode || forcedInvertedRegionSelectionMode
    readonly property bool readerBackgroundVisible: !recolor && readerBackgroundImageSource.toString().length > 0
    readonly property color readerBackgroundBaseColor: recolor ? backgroundColor : inverted ? "black" : "white"
    readonly property color readerBackgroundOverlayColor: inverted ? Qt.rgba(0, 0, 0, 0.86) : Qt.rgba(1, 1, 1, 0.86)

    signal zoomRequested(factor: real)
    signal translateSelectionRequested(selectedText: string)
    signal notInvertedRegionSelected(page: int, rect: var)
    signal forcedInvertedRegionSelected(page: int, rect: var)
    signal preserveColorForPageRequested(page: int)
    signal addPageToTableOfContentsRequested(page: int)
    signal chooseRectangleAreaRequested()
    signal chooseOvalAreaRequested()
    signal chooseInvertRectangleAreaRequested()
    signal clearPreservedAreasForPageRequested(page: int)
    signal clearInvertedAreasForPageRequested(page: int)
    signal sectionNavigationRequested(previous: bool)
    signal historyNavigationRequested(back: bool)
    signal linkActivated(location: var, sourceLocation: var)
    signal externalLinkActivated(url: string, sourceLocation: string)

    function hasLinkAtRootPosition(x, y) {
        return firstPageSurface.hasLinkAtRootPosition(x, y)
            || secondPageSurface.hasLinkAtRootPosition(x, y);
    }

    function clampPage(page) {
        if (!document || document.pageCount <= 0) {
            return 0;
        }

        const pageNumber = Number(page);
        return Math.max(0, Math.min(isNaN(pageNumber) ? 0 : pageNumber, document.pageCount - 1));
    }

    function displayPage(page) {
        const boundedPage = clampPage(page);
        if (!root.twoPageMode) {
            return boundedPage;
        }

        if (root.coverPageMode && boundedPage === 0) {
            return 0;
        }

        const segmentStart = spreadSegmentStartForPage(boundedPage);
        const firstContentPage = root.coverPageMode ? 1 : 0;
        const effectiveSegmentStart = Math.max(firstContentPage, segmentStart);
        return effectiveSegmentStart + Math.floor((boundedPage - effectiveSegmentStart) / 2) * 2;
    }

    function goToPage(page) {
        const boundedPage = displayPage(page);
        if (currentPage !== boundedPage) {
            currentPage = boundedPage;
        }
        if (document) {
            document.currentPage = boundedPage;
        }
        Qt.callLater(positionPage);
    }

    function goToPagePosition(page, normalizedX, normalizedY, position) {
        const targetPage = clampPage(page);
        const x = Number(normalizedX);
        const y = Number(normalizedY);
        if (isFinite(x) && isFinite(y)) {
            pendingViewportPosition = {
                page: targetPage,
                x: Math.max(0, Math.min(1, x)),
                y: Math.max(0, Math.min(1, y)),
                position: position === "topLeft" ? "topLeft" : "center"
            };
        } else {
            pendingViewportPosition = null;
        }
        goToPage(targetPage);
    }

    function goToPreviousPage() {
        goToPage(root.twoPageMode ? root.spreadFirstPage - 1 : currentPage - 1);
    }

    function goToNextPage() {
        if (!root.document || root.document.pageCount <= 0) {
            return;
        }

        if (!root.twoPageMode) {
            goToPage(currentPage + 1);
            return;
        }

        const nextPage = root.spreadSecondPage >= 0 ? root.spreadSecondPage + 1 : root.spreadFirstPage + 1;
        goToPage(nextPage);
    }

    function hasPreviousPage() {
        return root.document && root.document.pageCount > 0 && root.spreadFirstPage > 0;
    }

    function hasNextPage() {
        if (!root.document || root.document.pageCount <= 0) {
            return false;
        }

        if (!root.twoPageMode) {
            return currentPage + 1 < root.document.pageCount;
        }

        const nextPage = root.spreadSecondPage >= 0 ? root.spreadSecondPage + 1 : root.spreadFirstPage + 1;
        return nextPage < root.document.pageCount;
    }

    function isSpreadBoundaryPage(page) {
        const boundedPage = clampPage(page);
        const pages = root.primaryTableOfContentsPages || [];
        for (let index = 0; index < pages.length; ++index) {
            if (Number(pages[index]) === boundedPage) {
                return true;
            }
        }
        return false;
    }

    function spreadSegmentStartForPage(page) {
        const boundedPage = clampPage(page);
        const pages = root.primaryTableOfContentsPages || [];
        let segmentStart = 0;
        for (let index = 0; index < pages.length; ++index) {
            const primaryPage = Number(pages[index]);
            if (isFinite(primaryPage) && primaryPage <= boundedPage) {
                segmentStart = Math.max(segmentStart, primaryPage);
            }
        }
        return segmentStart;
    }

    function secondPageForSpread(firstPage) {
        if (!root.document || root.document.pageCount <= 0) {
            return -1;
        }

        const secondPage = firstPage + 1;
        if (root.coverPageMode && firstPage === 0) {
            return -1;
        }
        if (secondPage >= root.document.pageCount || isSpreadBoundaryPage(secondPage)) {
            return -1;
        }
        return secondPage;
    }

    function searchBack() {
    }

    function searchForward() {
    }

    function positionPage() {
        panDampingTimer.stop();
        if (applyPendingViewportPosition()) {
            return;
        }
        flickable.returnToBounds();
    }

    function pageSurfaceForPage(page) {
        if (firstPageSurface.valid && firstPageSurface.page === page) {
            return firstPageSurface;
        }
        if (secondPageSurface.valid && secondPageSurface.page === page) {
            return secondPageSurface;
        }
        return null;
    }

    function clampedContentX(value) {
        const maximumX = Math.max(0, flickable.contentWidth - flickable.width);
        return Math.max(0, Math.min(maximumX, value));
    }

    function clampedContentY(value) {
        const maximumY = Math.max(0, flickable.contentHeight - flickable.height);
        return Math.max(0, Math.min(maximumY, value));
    }

    function applyPendingViewportPosition() {
        const target = pendingViewportPosition;
        pendingViewportPosition = null;
        if (!target) {
            return false;
        }

        const targetSurface = pageSurfaceForPage(target.page);
        if (!targetSurface) {
            flickable.returnToBounds();
            return true;
        }

        const targetX = pagesRow.x + targetSurface.x + target.x * targetSurface.width;
        const targetY = pagesRow.y + targetSurface.y + target.y * targetSurface.height;
        const viewportOffsetX = target.position === "topLeft" ? 0 : flickable.width / 2;
        const viewportOffsetY = target.position === "topLeft" ? 0 : flickable.height / 2;

        flickable.contentX = clampedContentX(targetX - viewportOffsetX);
        flickable.contentY = clampedContentY(targetY - viewportOffsetY);
        return true;
    }


    function wheelNavigationDelta(wheel) {
        const buttons = Number(wheel.buttons) || 0;
        if (buttons & Qt.BackButton) {
            return {
                value: 1,
                vertical: false
            };
        }
        if (buttons & Qt.ForwardButton) {
            return {
                value: -1,
                vertical: false
            };
        }

        const angleDelta = wheel.angleDelta || Qt.point(0, 0);
        const pixelDelta = wheel.pixelDelta || Qt.point(0, 0);
        const angleDx = Number(angleDelta.x) || 0;
        const angleDy = Number(angleDelta.y) || 0;
        const pixelDx = Number(pixelDelta.x) || 0;
        const pixelDy = Number(pixelDelta.y) || 0;
        const dx = angleDx !== 0 ? angleDx : pixelDx;
        const dy = angleDy !== 0 ? angleDy : pixelDy;
        const horizontal = angleDx !== 0 || pixelDx !== 0;
        return {
            value: horizontal ? dx : dy,
            vertical: !horizontal
        };
    }

    function scrollByWheel(wheel) {
        const navigationDelta = wheelNavigationDelta(wheel);
        const delta = navigationDelta.value;
        if (Math.abs(delta) < 1) {
            return false;
        }

        if (!navigationDelta.vertical) {
            return root.handleSectionWheel(delta);
        }

        if (delta > 0) {
            goToPreviousPage();
        } else {
            goToNextPage();
        }
        return true;
    }

    function handleSectionWheel(delta) {
        if (Math.abs(delta) < 1) {
            return false;
        }

        root.sectionNavigationRequested(delta > 0);
        return true;
    }

    function copySelectionToClipboard(removeLineBreaks) {
        if (document) {
            document.copySelectedText(removeLineBreaks);
        }
    }

    function clearSelection() {
        if (document) {
            document.clearTextSelection();
        }
    }

    function translateSelection() {
        if (!document || !document.hasTextSelection) {
            return;
        }

        const selectedText = document.selectedTextForTranslation().trim();
        if (selectedText.length === 0) {
            return;
        }

        root.translateSelectionRequested(selectedText);
    }

    function openSelectionMenu(x, y) {
        selectionMenu.x = Math.max(0, Math.min(width - selectionMenu.implicitWidth, x));
        selectionMenu.y = Math.max(0, Math.min(height - selectionMenu.implicitHeight, y));
        selectionMenu.open();
    }

    function openPageColorMenu(page, x, y) {
        pageColorMenu.page = page;
        pageColorMenu.x = Math.max(0, Math.min(width - pageColorMenu.implicitWidth, x));
        pageColorMenu.y = Math.max(0, Math.min(height - pageColorMenu.implicitHeight, y));
        pageColorMenu.open();
    }

    onCurrentPageChanged: {
        if (document) {
            document.currentPage = clampPage(currentPage);
        }
    }

    onRenderScaleChanged: Qt.callLater(positionPage)
    onTwoPageModeChanged: Qt.callLater(positionPage)
    onPrimaryTableOfContentsPagesChanged: root.goToPage(root.currentPage)

    Connections {
        target: root.document

        function onOpenedChanged() {
            root.goToPage(root.currentPage);
        }

        function onPageCountChanged() {
            root.goToPage(root.currentPage);
        }
    }

    QtObject {
        id: searchState

        property int count: 0
        property int currentResult: 0
        readonly property var currentResultLink: null
    }

    Rectangle {
        anchors.fill: parent
        color: root.readerBackgroundBaseColor
    }

    Image {
        anchors.fill: parent
        visible: root.readerBackgroundVisible
        source: root.readerBackgroundImageSource
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: false
        smooth: true
    }

    Rectangle {
        anchors.fill: parent
        visible: root.readerBackgroundVisible
        color: root.readerBackgroundOverlayColor
    }

    Flickable {
        id: flickable

        anchors.fill: parent
        clip: true
        interactive: false
        boundsBehavior: Flickable.StopAtBounds
        contentWidth: Math.max(width, pagesRow.width + Kirigami.Units.gridUnit * 2)
        contentHeight: Math.max(height, pagesRow.height + Kirigami.Units.gridUnit * 2)

        Row {
            id: pagesRow

            spacing: root.twoPageMode ? 4 * Kirigami.Units.largeSpacing : 0
            x: Math.max(Kirigami.Units.gridUnit, (flickable.width - width) / 2)
            y: Math.max(Kirigami.Units.gridUnit, (flickable.height - height) / 2)

            PageSurface {
                id: firstPageSurface

                page: root.twoPageMode ? root.spreadFirstPage : root.currentPage
            }

            PageSurface {
                id: secondPageSurface

                page: root.spreadSecondPage
            }
        }
    }

    MouseArea {
        id: wheelNavigationArea

        property bool hoveringLink: false

        anchors.fill: parent
        acceptedButtons: Qt.BackButton | Qt.ForwardButton
        hoverEnabled: true
        cursorShape: hoveringLink ? Qt.PointingHandCursor : Qt.ArrowCursor
        propagateComposedEvents: true
        z: 10

        function updateHoveredLink(x, y) {
            const rootPosition = wheelNavigationArea.mapToItem(root, x, y);
            hoveringLink = root.hasLinkAtRootPosition(rootPosition.x, rootPosition.y);
        }

        onPositionChanged: mouse => updateHoveredLink(mouse.x, mouse.y)
        onExited: hoveringLink = false

        onClicked: mouse => {
            if (mouse.button === Qt.BackButton) {
                root.historyNavigationRequested(true);
                mouse.accepted = true;
            } else if (mouse.button === Qt.ForwardButton) {
                root.historyNavigationRequested(false);
                mouse.accepted = true;
            }
        }

        onWheel: wheel => {
            if (wheel.modifiers & Qt.ControlModifier) {
                const angleDelta = wheel.angleDelta || Qt.point(0, 0);
                const pixelDelta = wheel.pixelDelta || Qt.point(0, 0);
                const dy = Number(angleDelta.y) || Number(pixelDelta.y) || 0;
                if (Math.abs(dy) >= 1) {
                    root.zoomRequested(dy > 0 ? root.zoomStep : 1 / root.zoomStep);
                    wheel.accepted = true;
                    return;
                }
            }

            wheel.accepted = root.scrollByWheel(wheel);
        }
    }

    MouseArea {
        id: contentDragArea

        property real dragStartX: 0
        property real dragStartY: 0
        property real dragStartContentX: 0
        property real dragStartContentY: 0
        property real targetContentX: 0
        property real targetContentY: 0
        property bool hoveringLink: false
        readonly property real dragDamping: 0.45
        readonly property real dragStopThreshold: 0.5
        readonly property bool canDragHorizontally: flickable.contentWidth > flickable.width + 0.5
        readonly property bool canDragVertically: flickable.contentHeight > flickable.height + 0.5
        readonly property bool canDragContent: canDragHorizontally || canDragVertically

        anchors.fill: flickable
        acceptedButtons: Qt.MiddleButton
        hoverEnabled: true
        cursorShape: pressed && canDragContent ? Qt.ClosedHandCursor : (hoveringLink ? Qt.PointingHandCursor : Qt.ArrowCursor)
        preventStealing: true
        propagateComposedEvents: true

        function updateHoveredLink(x, y) {
            const rootPosition = contentDragArea.mapToItem(root, x, y);
            hoveringLink = firstPageSurface.hasLinkAtRootPosition(rootPosition.x, rootPosition.y)
                || secondPageSurface.hasLinkAtRootPosition(rootPosition.x, rootPosition.y);
        }

        function clampedContentX(value) {
            const maximumX = Math.max(0, flickable.contentWidth - flickable.width);
            return Math.max(0, Math.min(maximumX, value));
        }

        function clampedContentY(value) {
            const maximumY = Math.max(0, flickable.contentHeight - flickable.height);
            return Math.max(0, Math.min(maximumY, value));
        }

        function dampedContentPosition(currentValue, targetValue) {
            const delta = targetValue - currentValue;
            return Math.abs(delta) <= contentDragArea.dragStopThreshold ? targetValue : currentValue + delta * contentDragArea.dragDamping;
        }

        function applyDampedPan() {
            if (!contentDragArea.canDragContent) {
                panDampingTimer.stop();
                return;
            }

            if (contentDragArea.canDragHorizontally) {
                flickable.contentX = contentDragArea.dampedContentPosition(flickable.contentX, contentDragArea.targetContentX);
            }
            if (contentDragArea.canDragVertically) {
                flickable.contentY = contentDragArea.dampedContentPosition(flickable.contentY, contentDragArea.targetContentY);
            }

            const horizontalSettled = !contentDragArea.canDragHorizontally || Math.abs(flickable.contentX - contentDragArea.targetContentX) <= contentDragArea.dragStopThreshold;
            const verticalSettled = !contentDragArea.canDragVertically || Math.abs(flickable.contentY - contentDragArea.targetContentY) <= contentDragArea.dragStopThreshold;
            if (horizontalSettled && verticalSettled) {
                if (contentDragArea.canDragHorizontally) {
                    flickable.contentX = contentDragArea.targetContentX;
                }
                if (contentDragArea.canDragVertically) {
                    flickable.contentY = contentDragArea.targetContentY;
                }
                panDampingTimer.stop();
            }
        }

        onPressed: mouse => {
            if (mouse.button !== Qt.MiddleButton) {
                mouse.accepted = false;
                return;
            }

            contentDragArea.dragStartX = mouse.x;
            contentDragArea.dragStartY = mouse.y;
            contentDragArea.dragStartContentX = flickable.contentX;
            contentDragArea.dragStartContentY = flickable.contentY;
            contentDragArea.targetContentX = flickable.contentX;
            contentDragArea.targetContentY = flickable.contentY;
            panDampingTimer.stop();
            mouse.accepted = true;
        }

        onPositionChanged: mouse => {
            contentDragArea.updateHoveredLink(mouse.x, mouse.y);

            if (!contentDragArea.pressed || !(mouse.buttons & Qt.MiddleButton) || !contentDragArea.canDragContent) {
                return;
            }

            if (contentDragArea.canDragHorizontally) {
                contentDragArea.targetContentX = contentDragArea.clampedContentX(contentDragArea.dragStartContentX - (mouse.x - contentDragArea.dragStartX));
            }
            if (contentDragArea.canDragVertically) {
                contentDragArea.targetContentY = contentDragArea.clampedContentY(contentDragArea.dragStartContentY - (mouse.y - contentDragArea.dragStartY));
            }
            contentDragArea.applyDampedPan();
            if (!panDampingTimer.running) {
                panDampingTimer.start();
            }
            mouse.accepted = true;
        }

        onReleased: mouse => {
            if (mouse.button === Qt.MiddleButton) {
                contentDragArea.updateHoveredLink(mouse.x, mouse.y);
                mouse.accepted = true;
            }
        }

        onExited: hoveringLink = false

        Timer {
            id: panDampingTimer

            interval: 16
            repeat: true
            onTriggered: contentDragArea.applyDampedPan()
        }

    }

    QQC2.Menu {
        id: selectionMenu

        onClosed: root.clearSelection()

        QQC2.MenuItem {
            text: i18n("Copy")
            icon.name: "edit-copy"
            enabled: root.document && root.document.hasTextSelection
            onClicked: root.copySelectionToClipboard(false)
        }

        QQC2.MenuItem {
            text: i18n("Copy Without Line Breaks")
            icon.name: "edit-copy"
            enabled: root.document && root.document.hasTextSelection
            onClicked: root.copySelectionToClipboard(true)
        }

        QQC2.MenuItem {
            text: i18n("Translate")
            icon.name: "edit-find-replace"
            enabled: root.document && root.document.hasTextSelection
            onClicked: root.translateSelection()
        }

    }

    QQC2.Menu {
        id: pageColorMenu

        property int page: -1
        readonly property int preservedAreaCount: {
            root.notInvertedRegionsVersion;
            return root.notInvertedRegionsForPage && pageColorMenu.page >= 0 ? root.notInvertedRegionsForPage(pageColorMenu.page).length : 0;
        }
        readonly property int invertedAreaCount: {
            root.notInvertedRegionsVersion;
            return root.forcedInvertedRegionsForPage && pageColorMenu.page >= 0 ? root.forcedInvertedRegionsForPage(pageColorMenu.page).length : 0;
        }

        QQC2.MenuItem {
            text: i18nc("@action:inmenu", "Add Page to Table of Contents")
            icon.name: "list-add"
            enabled: pageColorMenu.page >= 0
            onClicked: root.addPageToTableOfContentsRequested(pageColorMenu.page)
        }

        QQC2.MenuItem {
            text: i18nc("@action:inmenu", "Normal Coloring for This Page")
            icon.name: "edit-select-all"
            enabled: pageColorMenu.page >= 0
            onClicked: root.preserveColorForPageRequested(pageColorMenu.page)
        }

        QQC2.MenuSeparator {}

        QQC2.MenuItem {
            text: i18nc("@action:inmenu", "Choose Rectangle Area for Normal Coloring")
            icon.name: "selection-start-symbolic"
            enabled: root.document && root.document.opened
            onClicked: root.chooseRectangleAreaRequested()
        }

        QQC2.MenuItem {
            text: i18nc("@action:inmenu", "Choose Oval Area for Normal Coloring")
            icon.name: "selection-start-symbolic"
            enabled: root.document && root.document.opened
            onClicked: root.chooseOvalAreaRequested()
        }

        QQC2.MenuItem {
            text: i18nc("@action:inmenu", "Choose Rectangle Area for Theme Coloring")
            icon.name: "selection-end-symbolic"
            enabled: root.document && root.document.opened
            onClicked: root.chooseInvertRectangleAreaRequested()
        }

        QQC2.MenuSeparator {}

        QQC2.MenuItem {
            text: i18nc("@action:inmenu", "Clear Preserved Areas on Page")
            icon.name: "edit-clear"
            enabled: pageColorMenu.preservedAreaCount > 0
            onClicked: root.clearPreservedAreasForPageRequested(pageColorMenu.page)
        }

        QQC2.MenuItem {
            text: i18nc("@action:inmenu", "Clear Inverted Areas on Page")
            icon.name: "edit-clear"
            enabled: pageColorMenu.invertedAreaCount > 0
            onClicked: root.clearInvertedAreasForPageRequested(pageColorMenu.page)
        }
    }

    component PageSurface: Item {
        id: surface

        property int page: -1
        readonly property bool valid: root.document && root.document.opened && page >= 0 && page < root.document.pageCount
        readonly property size pagePointSize: valid ? root.document.pagePointSize(page) : Qt.size(1, 1)
        readonly property real coverScale: root.coverPageMode && page === 0 && root.document && root.document.pageCount > 1
            ? root.document.pagePointSize(1).height / Math.max(1, pagePointSize.height)
            : 1
        property point regionSelectionStart: Qt.point(0, 0)
        property rect activeRegionSelection: Qt.rect(0, 0, 0, 0)
        readonly property var notInvertedRegions: {
            root.notInvertedRegionsVersion;
            return root.notInvertedRegionsForPage && surface.valid ? root.notInvertedRegionsForPage(surface.page) : [];
        }
        readonly property var forcedInvertedRegions: {
            root.notInvertedRegionsVersion;
            return root.forcedInvertedRegionsForPage && surface.valid ? root.forcedInvertedRegionsForPage(surface.page) : [];
        }
        readonly property color preserveRegionColor: Kirigami.Theme.highlightColor
        readonly property color forcedInvertRegionColor: "#c64600"
        readonly property color activeRegionColor: root.forcedInvertedRegionSelectionMode ? forcedInvertRegionColor : preserveRegionColor

        visible: valid
        clip: true
        width: valid ? Math.max(1, pagePointSize.width * root.renderScale * coverScale) : 0
        height: valid ? Math.max(1, pagePointSize.height * root.renderScale * coverScale) : 0

        function clampedPoint(x, y) {
            return Qt.point(Math.max(0, Math.min(surface.width, x)), Math.max(0, Math.min(surface.height, y)));
        }

        function selectionRect(start, end) {
            const left = Math.min(start.x, end.x);
            const top = Math.min(start.y, end.y);
            const right = Math.max(start.x, end.x);
            const bottom = Math.max(start.y, end.y);
            return Qt.rect(left, top, right - left, bottom - top);
        }

        function normalizedSelectionRect(rect) {
            if (surface.width <= 0 || surface.height <= 0) {
                return Qt.rect(0, 0, 0, 0);
            }

            return Qt.rect(rect.x / surface.width, rect.y / surface.height, rect.width / surface.width, rect.height / surface.height);
        }

        function canvasColor(color, opacity) {
            return "rgba(" + Math.round(color.r * 255) + "," + Math.round(color.g * 255) + "," + Math.round(color.b * 255) + "," + (color.a * opacity).toFixed(3) + ")";
        }

        function hasLinkAtRootPosition(x, y) {
            const position = pageItem.mapFromItem(root, x, y);
            return position.x >= 0 && position.y >= 0 && position.x <= pageItem.width && position.y <= pageItem.height
                && pageItem.hasLinkAt(position.x, position.y);
        }

        Rectangle {
            anchors.fill: parent
            color: root.readerBackgroundBaseColor
        }

        Image {
            anchors.fill: parent
            visible: root.readerBackgroundVisible
            source: root.readerBackgroundImageSource
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            cache: false
            smooth: true
        }

        Rectangle {
            anchors.fill: parent
            visible: root.readerBackgroundVisible
            color: root.readerBackgroundOverlayColor
        }

        OkularPdfPageItem {
            id: pageItem

            anchors.fill: parent
            document: root.document
            pageNumber: surface.valid ? surface.page : -1
            inverted: root.inverted && !root.recolor
            recolor: root.recolor
            transparentPageBackground: root.readerBackgroundVisible
            foregroundColor: root.foregroundColor
            backgroundColor: root.backgroundColor
            notInvertedRegions: surface.notInvertedRegions
            forcedInvertedRegions: surface.forcedInvertedRegions

            onSelectionMenuRequested: (x, y) => {
                const mappedPosition = pageItem.mapToItem(root, x, y);
                root.openSelectionMenu(mappedPosition.x, mappedPosition.y);
            }

            onPageColorMenuRequested: (x, y) => {
                const mappedPosition = pageItem.mapToItem(root, x, y);
                root.openPageColorMenu(surface.page, mappedPosition.x, mappedPosition.y);
            }

            onLinkActivated: (location, sourceLocation) => root.linkActivated(location, sourceLocation)
            onExternalLinkActivated: (url, sourceLocation) => root.externalLinkActivated(url, sourceLocation)
        }

        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.color: Kirigami.Theme.disabledTextColor
            border.width: 1
            z: 1
        }

        Repeater {
            model: root.colorRegionSelectionMode ? surface.notInvertedRegions : []

            Item {
                x: Math.max(0, Number(modelData.x) * surface.width)
                y: Math.max(0, Number(modelData.y) * surface.height)
                width: Math.max(1, Number(modelData.width) * surface.width)
                height: Math.max(1, Number(modelData.height) * surface.height)
                z: 2

                readonly property bool elliptical: modelData.shape === "ellipse"

                Rectangle {
                    anchors.fill: parent
                    visible: !parent.elliptical
                    color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.10)
                    border.color: Kirigami.Theme.highlightColor
                    border.width: 2
                }

                Canvas {
                    anchors.fill: parent
                    visible: parent.elliptical
                    readonly property color highlightColor: Kirigami.Theme.highlightColor
                    onPaint: {
                        const context = getContext("2d");
                        context.reset();
                        context.beginPath();
                        context.ellipse(1, 1, Math.max(0, width - 2), Math.max(0, height - 2));
                        context.fillStyle = surface.canvasColor(highlightColor, 0.10);
                        context.fill();
                        context.strokeStyle = surface.canvasColor(highlightColor, 1);
                        context.lineWidth = 2;
                        context.stroke();
                    }
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onVisibleChanged: requestPaint()
                    onHighlightColorChanged: requestPaint()
                }
            }
        }

        Repeater {
            model: root.colorRegionSelectionMode ? surface.forcedInvertedRegions : []

            Rectangle {
                x: Math.max(0, Number(modelData.x) * surface.width)
                y: Math.max(0, Number(modelData.y) * surface.height)
                width: Math.max(1, Number(modelData.width) * surface.width)
                height: Math.max(1, Number(modelData.height) * surface.height)
                color: Qt.rgba(0.78, 0.27, 0, 0.13)
                border.color: surface.forcedInvertRegionColor
                border.width: 2
                z: 2
            }
        }

        Rectangle {
            x: surface.activeRegionSelection.x
            y: surface.activeRegionSelection.y
            width: surface.activeRegionSelection.width
            height: surface.activeRegionSelection.height
            visible: root.colorRegionSelectionMode && !root.ovalNotInvertedRegionSelectionMode && width > 0 && height > 0
            color: root.forcedInvertedRegionSelectionMode ? Qt.rgba(0.78, 0.27, 0, 0.18) : Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.16)
            border.color: surface.activeRegionColor
            border.width: 2
            z: 3
        }

        Canvas {
            x: surface.activeRegionSelection.x
            y: surface.activeRegionSelection.y
            width: surface.activeRegionSelection.width
            height: surface.activeRegionSelection.height
            visible: root.ovalNotInvertedRegionSelectionMode && width > 0 && height > 0
            z: 3
            readonly property color selectionColor: surface.activeRegionColor

            onPaint: {
                const context = getContext("2d");
                context.reset();
                context.beginPath();
                context.ellipse(1, 1, Math.max(0, width - 2), Math.max(0, height - 2));
                context.fillStyle = surface.canvasColor(selectionColor, 0.16);
                context.fill();
                context.strokeStyle = surface.canvasColor(selectionColor, 1);
                context.lineWidth = 2;
                context.stroke();
            }
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onVisibleChanged: requestPaint()
            onSelectionColorChanged: requestPaint()
        }

        MouseArea {
            id: notInvertedRegionMouseArea

            anchors.fill: parent
            visible: root.colorRegionSelectionMode
            enabled: visible
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.CrossCursor
            preventStealing: true
            z: 4

            onPressed: mouse => {
                const start = surface.clampedPoint(mouse.x, mouse.y);
                surface.regionSelectionStart = start;
                surface.activeRegionSelection = Qt.rect(start.x, start.y, 0, 0);
                mouse.accepted = true;
            }

            onPositionChanged: mouse => {
                if (!notInvertedRegionMouseArea.pressed) {
                    return;
                }

                const end = surface.clampedPoint(mouse.x, mouse.y);
                surface.activeRegionSelection = surface.selectionRect(surface.regionSelectionStart, end);
                mouse.accepted = true;
            }

            onReleased: mouse => {
                const end = surface.clampedPoint(mouse.x, mouse.y);
                const selection = surface.selectionRect(surface.regionSelectionStart, end);
                surface.activeRegionSelection = Qt.rect(0, 0, 0, 0);
                mouse.accepted = true;
                if (selection.width < 4 || selection.height < 4) {
                    return;
                }

                const coarseSelection = surface.normalizedSelectionRect(selection);
                const snapDisabled = (mouse.modifiers & Qt.ShiftModifier) !== 0;
                const refinedSelection = snapDisabled ? coarseSelection : pageItem.refinedColorRegion(coarseSelection);
                const normalizedSelection = root.ovalNotInvertedRegionSelectionMode
                    ? {
                        x: refinedSelection.x,
                        y: refinedSelection.y,
                        width: refinedSelection.width,
                        height: refinedSelection.height,
                        shape: "ellipse"
                    }
                    : refinedSelection;
                if (root.forcedInvertedRegionSelectionMode) {
                    root.forcedInvertedRegionSelected(surface.page, normalizedSelection);
                } else {
                    root.notInvertedRegionSelected(surface.page, normalizedSelection);
                }
            }
        }
    }
}
