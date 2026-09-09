// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtWebEngine
import QtWebChannel
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.delegates as Delegates
import org.kde.arianna
import Qt.labs.platform

Kirigami.Page {
    id: root

    property string url
    property string filename
    property string locations
    property string currentLocation
    property real currentProgress: 0
    property real zoomLevel: 1.0
    property string pageMode: ""
    property var reloadLocation: null
    property int bookDeliveryReloadSerial: 0
    property string bookDeliveryVersion: ""
    property string pendingInvalidImportSessionId: ""
    property string pendingActiveFileImportBookId: ""
    property string pendingInvalidImportWorkingCopyPath: ""
    property string pendingInvalidImportMessage: ""
    property var pendingInvalidImportIssues: []
    property var entry: null
    property bool readOnly: false
    property bool readerSuspended: false
    property bool translationPending: false
    property string referenceSourceTitle: ""
    readonly property color readerTheme: Kirigami.Theme.backgroundColor
    readonly property bool hideSidebar: true
    readonly property bool centerToolbarActions: true
    readonly property string defaultPageMode: Config.readerPageMode === 0 ? "single" : "two"
    readonly property string effectivePageMode: root.pageMode === "single" || root.pageMode === "two" ? root.pageMode : root.defaultPageMode
    readonly property int readerPageColumnCount: root.effectivePageMode === "single" ? 1 : 2
    readonly property bool activeLayerPage: applicationWindow() ? applicationWindow().pageStack.layers.currentItem === root : false
    property int currentReaderTheme: Config.readerTheme
    readonly property string searchResultOutlineColor: currentReaderTheme === 1 ? "#ff4444" : currentReaderTheme === 2 ? "#44cc44" : '#1a52b3'

    property var layouts: {
        'auto': {
            'renderTo': "'viewer'",
            'options': {
                width: '100%',
                flow: 'paginated',
                maxSpreadColumns: 2
            }
        },
        'single': {
            renderTo: "'viewer'",
            options: {
                width: '100%',
                flow: 'paginated',
                spread: 'none'
            }
        },
        'scrolled': {
            renderTo: 'document.body',
            options: {
                width: '100%',
                flow: 'scrolled-doc'
            }
        },
        'continuous': {
            renderTo: 'document.body',
            options: {
                width: '100%',
                flow: 'scrolled',
                manager: 'continuous'
            }
        }
    }

    signal relocated(newLocation: var, newProgress: int)
    signal locationsLoaded(locations: var)
    signal zoomLevelSaved(zoomLevel: real)
    signal pageModeSaved(pageMode: string)
    signal bookReady(title: var)
    signal bookClosed
    signal addToLibraryRequested(var callback)

    onCurrentReaderThemeChanged: backend.applyStyle()
    onEffectivePageModeChanged: backend.applyStyle()

    function setReaderThemeMode(mode) {
        if (currentReaderTheme === mode && Config.readerTheme === mode) {
            return;
        }

        currentReaderTheme = mode;
        Config.readerTheme = mode;
        Config.save();
    }

    function cycleReaderThemeMode() {
        setReaderThemeMode((currentReaderTheme + 1) % 3);
    }

    function normalizedPageMode(mode) {
        return mode === "single" || mode === "two" ? mode : root.defaultPageMode;
    }

    function currentPageMode() {
        return root.effectivePageMode;
    }

    function updateCurrentTableOfContentsItem(tocItem) {
        const window = applicationWindow();
        if (!window || !window.contextDrawer) {
            return;
        }

        window.contextDrawer.currentTocId = tocItem && tocItem.id !== undefined && tocItem.id !== null ? String(tocItem.id) : "";
        window.contextDrawer.currentHref = tocItem && tocItem.href ? String(tocItem.href) : "";
    }

    function readerLayoutForMode(mode) {
        const normalizedMode = root.normalizedPageMode(mode);
        const maxWidth = Config.maxWidth || 720;
        return {
            gap: 0.06,
            maxInlineSize: maxWidth,
            maxBlockSize: maxWidth * 2,
            maxColumnCount: normalizedMode === "single" ? 1 : 2,
            selectionAutoTurn: normalizedMode === "single",
            flow: "paginated",
            animated: true
        };
    }

    function currentReaderLayout() {
        return root.readerLayoutForMode(root.currentPageMode());
    }

    function updateLocalEntryPageMode(mode) {
        if (!root.entry) {
            return;
        }

        root.entry = Object.assign({}, root.entry, { pageMode: mode });
    }

    function setPageMode(mode, persist) {
        const normalizedMode = root.normalizedPageMode(mode);
        const pageModeChanged = root.pageMode !== normalizedMode;
        if (pageModeChanged) {
            root.pageMode = normalizedMode;
        } else {
            backend.applyStyle();
        }
        if (persist) {
            root.updateLocalEntryPageMode(normalizedMode);
            root.pageModeSaved(normalizedMode);
        }
    }

    function bookServerIdentifier() {
        return root.entry ? (root.entry.uniqueIdentifier || root.entry.identifier || "") : "";
    }

    function bookDeliveryMode(value, fallbackValue, allowedValues) {
        const normalized = (value || fallbackValue).trim().toLowerCase();
        return allowedValues.indexOf(normalized) !== -1 ? normalized : fallbackValue;
    }

    function trimmedConfigValue(value) {
        return (value || "").trim();
    }

    function hasEditorStartupContext(selectedText, cfi, anchorId, anchorOpenTag, anchorCloseTag) {
        return trimmedConfigValue(selectedText).length > 0
            || trimmedConfigValue(cfi).length > 0
            || trimmedConfigValue(anchorId).length > 0
            || trimmedConfigValue(anchorOpenTag).length > 0
            || trimmedConfigValue(anchorCloseTag).length > 0;
    }

    function editorCommandForContext(withStartupContext) {
        const command = withStartupContext ? trimmedConfigValue(Config.editorStartWithTextCommand) : trimmedConfigValue(Config.editorCommand);
        return command.length > 0 ? command : trimmedConfigValue(Config.editorPath);
    }

    function canEditBookWithContext(withStartupContext) {
        return editorCommandForContext(withStartupContext).length > 0 && root.filename !== "";
    }

    function bumpBookDeliveryVersion(version) {
        bookDeliveryReloadSerial += 1;
        bookDeliveryVersion = version && version.length > 0 ? version : Date.now() + "-" + bookDeliveryReloadSerial;
    }

    function editBook(selectedText, cfi, anchorId, anchorOpenTag, anchorCloseTag) {
        const withStartupContext = hasEditorStartupContext(selectedText, cfi, anchorId, anchorOpenTag, anchorCloseTag);
        const editorAppFilePath = editorCommandForContext(withStartupContext);
        if (editorAppFilePath.length === 0 || !root.filename) {
            return false;
        }

        const session = EditorSessionStore.createOrReuseEditorSession(bookServerIdentifier(), root.filename);
        if (!session || session.error) {
            console.warn("Unable to create editor session:", session ? session.message || "" : "");
            return false;
        }

        const editorArguments = [session.workingCopyPath];
        if (cfi && cfi.length > 0) {
            editorArguments.push("--goto-cfi");
            editorArguments.push(cfi);
        }
        if (anchorId && anchorId.length > 0) {
            editorArguments.push("--anchor-id");
            editorArguments.push(anchorId);
        }
        if (anchorOpenTag && anchorOpenTag.length > 0) {
            editorArguments.push("--anchor-open-tag");
            editorArguments.push(anchorOpenTag);
        }
        if (anchorCloseTag && anchorCloseTag.length > 0) {
            editorArguments.push("--anchor-close-tag");
            editorArguments.push(anchorCloseTag);
        }
        if (selectedText && selectedText.length > 0) {
            editorArguments.push("--select-text");
            editorArguments.push(selectedText);
        }
        if (session.reused && ExternalProcess.isEditorSessionRunning(session.sessionId)) {
            if (!ExternalProcess.activateWindowForFile(session.workingCopyPath)) {
                console.warn("Unable to activate existing editor window for:", session.workingCopyPath);
            }
            return true;
        }

        if (!ExternalProcess.startEditorSession(session.sessionId, editorAppFilePath, editorArguments)) {
            if (!session.reused) {
                EditorSessionStore.finishSession(session.sessionId);
            }
            if (!ExternalProcess.activateWindowForFile(session.workingCopyPath)) {
                console.warn("Unable to start or activate editor for:", session.workingCopyPath);
            }
            return false;
        }
        return true;
    }

    function validationIssueSummary(issue) {
        if (!issue) {
            return "";
        }

        const parts = [];
        if (issue.type)
            parts.push(issue.type);
        if (issue.anchorId)
            parts.push(issue.anchorId);
        return parts.length > 0 ? parts.join(": ") : (issue.message || "");
    }

    function handleEditorImportStatus(sessionId, status, message, workingCopyPath, newStateId, anchorValidationIssues) {
        switch (status) {
        case "imported":
            console.log("Editor working copy imported as state:", newStateId);
            pendingInvalidImportSessionId = "";
            pendingActiveFileImportBookId = "";
            pendingInvalidImportWorkingCopyPath = "";
            pendingInvalidImportMessage = "";
            pendingInvalidImportIssues = [];
            bumpBookDeliveryVersion(newStateId);
            reloadChangedBookTimer.restart();
            break;
        case "unchanged":
            console.log("Editor working copy unchanged");
            break;
        case "invalid-epub":
        case "commit-failed":
            console.warn("Editor import failed:", status, message, "Recovery copy:", workingCopyPath);
            break;
        case "anchor-validation-failed":
            pendingInvalidImportSessionId = sessionId;
            pendingActiveFileImportBookId = "";
            pendingInvalidImportWorkingCopyPath = workingCopyPath || "";
            pendingInvalidImportMessage = message || "";
            pendingInvalidImportIssues = anchorValidationIssues || [];
            invalidAnchorImportDialog.open();
            break;
        default:
            console.warn("Editor import returned unknown status:", status, message, "Recovery copy:", workingCopyPath);
            break;
        }
    }

    function requestActiveFileImport(bookId) {
        if (!bookId || bookId.length === 0) {
            return;
        }

        const request = new XMLHttpRequest();
        request.open("POST", bookServerBaseUrl + "/" + encodeURIComponent(bookId) + "/state/import-active-file");
        request.setRequestHeader("Content-Type", "application/json");
        request.setRequestHeader("X-Arianna-Session-Token", bookServerSessionToken);
        request.onreadystatechange = function () {
            if (request.readyState !== 4) {
                return;
            }

            let response = null;
            try {
                response = request.responseText ? JSON.parse(request.responseText) : null;
            } catch (e) {
                response = null;
            }

            if (request.status >= 200 && request.status < 300) {
                root.pendingActiveFileImportBookId = "";
                root.pendingInvalidImportMessage = "";
                root.pendingInvalidImportIssues = [];
                root.bumpBookDeliveryVersion(response ? response.newStateId || "" : "");
                reloadChangedBookTimer.restart();
                return;
            }

            if (request.status === 409 && response) {
                root.pendingInvalidImportMessage = response.message || "";
                root.pendingInvalidImportIssues = response.anchorValidationIssues || [];
                invalidAnchorImportDialog.open();
                return;
            }

            console.warn("Unable to import active EPUB state:", request.status, request.responseText);
        };
        request.send(JSON.stringify({ allowInvalidAnchors: true }));
    }

    function reloadBook() {
        const serverIdentifier = bookServerIdentifier();
        if (!root.url || view.loading || !view.readerReady || !serverIdentifier) {
            return;
        }
        // HACK: renderTo and options are the value of layouts.auto, but referencing layouts.auto here crashes
        const renderTo = "'viewer'";
        const options = JSON.stringify({
            width: '100%',
            flow: 'paginated',
            maxSpreadColumns: 2
        });

        const resourceMode = bookDeliveryMode(Config.bookResourceMode, "auto", ["auto", "include", "outsource"]);
        const referencingMode = bookDeliveryMode(Config.bookReferencingMode, "reader", ["reader", "include", "none"]);
        const bookUrl = bookServerBaseUrl + "/" + encodeURIComponent(serverIdentifier)
            + "/book.epub?resourceMode=" + encodeURIComponent(resourceMode)
            + "&referencingMode=" + encodeURIComponent(referencingMode)
            + (bookDeliveryVersion.length > 0 ? "&v=" + encodeURIComponent(bookDeliveryVersion) : "");
        const urlNormalized = JSON.stringify(bookUrl);
        const serverIdentifierNormalized = JSON.stringify(serverIdentifier);
        console.info("opening book", urlNormalized, renderTo, options);
        const initLocation = reloadLocation ? reloadLocation : currentLocation;
        const initCfi = initLocation ? JSON.stringify(initLocation) : "null";
        const initLayout = JSON.stringify(root.currentReaderLayout());
        reloadLocation = null;
        view.bookReady = false;
        view.runJavaScript(`openSync(${urlNormalized}, ${initCfi}, ${serverIdentifierNormalized}, ${initLayout})`);
    }

    function reloadCurrentBook() {
        root.readerSuspended = false;
        bumpBookDeliveryVersion("");
        if (backend.location && backend.location.cfi) {
            root.currentLocation = backend.location.cfi;
            root.reloadLocation = backend.location.cfi;
        }

        if (view.loading) {
            reloadChangedBookTimer.restart();
            return;
        }

        view.closeReader();
        view.reload();
    }

    function suspendReaderForReference(reason) {
        if (backend.location && backend.location.cfi) {
            root.currentLocation = backend.location.cfi;
            root.reloadLocation = backend.location.cfi;
        } else if (root.currentLocation) {
            root.reloadLocation = root.currentLocation;
        }

        view.closeReader();
        root.readerSuspended = true;
    }

    function resumeSuspendedReader() {
        if (!root.readerSuspended || !root.url) {
            return;
        }

        root.readerSuspended = false;
        if (!root.reloadLocation && root.currentLocation) {
            root.reloadLocation = root.currentLocation;
        }

        view.reload();
    }

    function reloadCurrentBookIfModified(payload) {
        const currentBookId = root.bookServerIdentifier();
        const modifiedBookIds = payload && Array.isArray(payload.modifiedBookIds) ? payload.modifiedBookIds : [];
        if (!currentBookId || modifiedBookIds.indexOf(currentBookId) === -1) {
            return;
        }

        root.reloadCurrentBook();
    }

    function normalizedMetadataList(value) {
        if (value === undefined || value === null) {
            return [];
        }

        let items = [];
        if (Array.isArray(value)) {
            items = value;
        } else if (typeof value === "string") {
            items = value.split(/[;,]/);
        } else if (typeof value.length === "number") {
            for (let i = 0; i < value.length; ++i) {
                items.push(value[i]);
            }
        } else {
            items = String(value).split(/[;,]/);
        }

        const result = [];
        const seen = {};
        for (const rawItem of items) {
            const item = String(rawItem).trim();
            if (item.length === 0) {
                continue;
            }

            const key = item.toLowerCase();
            if (seen[key]) {
                continue;
            }

            seen[key] = true;
            result.push(item);
        }

        return result;
    }

    function isCurrentBookEntry(entry) {
        if (!entry) {
            return false;
        }

        const entryId = entry.uniqueIdentifier || entry.identifier || "";
        const currentId = root.bookServerIdentifier();
        if (entryId.length > 0 && currentId.length > 0 && entryId === currentId) {
            return true;
        }

        return root.filename.length > 0 && entry.filename === root.filename;
    }

    function applyUpdatedBookEntry(entry) {
        if (!root.isCurrentBookEntry(entry)) {
            return;
        }

        root.entry = entry;
        if (entry.pageMode) {
            root.pageMode = root.normalizedPageMode(entry.pageMode);
        }

        const topics = root.normalizedMetadataList(entry.genres);
        if (backend.metadata) {
            const metadata = Object.assign({}, backend.metadata);
            if (topics.length > 0) {
                metadata.subject = topics.map(topic => ({ name: topic }));
            } else {
                delete metadata.subject;
            }
            backend.metadata = metadata;
        }

        if (view.bookReady) {
            const subjectJson = JSON.stringify(topics.map(topic => ({ name: topic })));
            view.runJavaScript(`if (globalThis.reader && globalThis.reader.book && globalThis.reader.book.metadata) { globalThis.reader.book.metadata.subject = ${subjectJson}; }`);
        }
    }

    function metadataText(value) {
        if (value === undefined || value === null) {
            return "";
        }
        if (Array.isArray(value)) {
            return value.map(item => root.metadataText(item)).filter(item => item.length > 0).join(", ");
        }
        if (typeof value === "object") {
            if (value.name !== undefined) {
                return root.metadataText(value.name);
            }
            if (value.value !== undefined) {
                return root.metadataText(value.value);
            }

            const objectValues = Object.keys(value).map(key => root.metadataText(value[key])).filter(item => item.length > 0);
            return objectValues.length > 0 ? objectValues[0] : "";
        }

        return String(value).trim();
    }

    function metadataTextList(value) {
        if (value === undefined || value === null) {
            return [];
        }
        if (Array.isArray(value)) {
            return value.map(item => root.metadataText(item)).filter(item => item.length > 0);
        }

        const text = root.metadataText(value);
        return text.length > 0 ? [text] : [];
    }

    function detailsMetadata() {
        const metadata = backend.metadata || {};
        const entry = root.entry || {};
        const filename = entry.filename || root.filename || "";
        const entryGenres = root.normalizedMetadataList(entry.genres);
        const subjects = entryGenres.length > 0 ? entryGenres : root.metadataTextList(metadata.subject);
        const filetitle = entry.filetitle || filename.split("/").pop();

        return {
            filename: filename,
            filetitle: filetitle,
            title: root.metadataText(metadata.title) || root.metadataText(entry.title) || filetitle,
            author: root.metadataTextList(entry.author).length > 0 ? root.metadataTextList(entry.author) : root.metadataTextList(metadata.author),
            description: root.metadataTextList(entry.description).length > 0 ? root.metadataTextList(entry.description) : root.metadataTextList(metadata.description),
            publisher: root.metadataText(entry.publisher) || root.metadataText(metadata.publisher),
            language: root.metadataText(entry.language) || root.metadataText(metadata.language),
            pubdate: root.metadataText(entry.pubdate) || root.metadataText(entry.published) || root.metadataText(metadata.published) || root.metadataText(metadata.modified),
            rights: root.metadataText(entry.rights) || root.metadataText(metadata.rights),
            thumbnail: entry.thumbnail || "",
            identifier: root.metadataText(metadata.identifier) || entry.identifier || "",
            uniqueIdentifier: entry.uniqueIdentifier || root.bookServerIdentifier(),
            source: root.metadataText(entry.source) || root.metadataText(metadata.source),
            genres: subjects,
            zoomLevel: entry.zoomLevel || root.zoomLevel || 1.0,
            pageMode: entry.pageMode || root.currentPageMode()
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
        const refreshedEntry = applicationWindow().bookListModel.refreshBookFromFile(root.filename);
        if (!root.readOnly && refreshedEntry.filename && refreshedEntry.filename.length > 0) {
            root.entry = refreshedEntry;
        }
    }

    title: backend.metadata ? backend.metadata.title + (root.referenceSourceTitle ? ' ← ' + root.referenceSourceTitle : '') : ''
    padding: 0

    onUrlChanged: reloadBook()
    onActiveLayerPageChanged: if (activeLayerPage) {
        resumeSuspendedReader();
    }
    onReaderThemeChanged: backend.applyStyle()

    Connections {
        target: Config
        function onReaderThemeChanged() {
            root.currentReaderTheme = Config.readerTheme;
        }

        function onReaderBackgroundPathChanged() {
            view.closeReader();
            view.reload();
        }

        function onReaderPageModeChanged() {
            if (root.pageMode.length === 0) {
                backend.applyStyle();
            }
        }

        function onBookResourceModeChanged() {
            root.reloadCurrentBook();
        }

        function onBookReferencingModeChanged() {
            root.reloadCurrentBook();
        }
    }

    Connections {
        target: applicationWindow().bookListModel

        function onEntryDataUpdated(entry) {
            root.applyUpdatedBookEntry(entry);
        }
    }

    Kirigami.SearchDialog {
        id: searchDialog

        onAccepted: if (text === '') {
            view.runJavaScript(`find.clearHighlight()`);
            searchResultModel.clear();
        } else {
            searchResultModel.search(text);
            searchResultModel.loading = true;
        }

        emptyText: if (searchResultModel.loading) {
            return i18n("Loading");
        } else if (count === 0 && searchDialog.text.length > 2) {
            return i18n("No search results");
        } else {
            return '';
        }

        model: searchResultModel

        delegate: Delegates.RoundedItemDelegate {
            id: searchDelegate

            required property string sectionMarkup
            required property string markup
            required property string cfi

            onClicked: {
                view.goTo(cfi);
                searchDialog.close();
            }

            contentItem: ColumnLayout {
                spacing: 0

                QQC2.Label {
                    Layout.fillWidth: true
                    text: searchDelegate.sectionMarkup
                    wrapMode: Text.WordWrap
                    font: Kirigami.Theme.smallFont
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    text: searchDelegate.markup
                    wrapMode: Text.WordWrap
                }
            }
            background: Rectangle {
                radius: Kirigami.Units.smallSpacing

                border.width: 2

                border.color: root.searchResultOutlineColor

                color: "transparent"
            }
        }

        Shortcut {
            sequence: "Ctrl+F"
            onActivated: {
                searchDialog.open();
            }
        }
    }

    ListModel {
        id: notesModel
    }

    ListModel {
        id: referenceOverviewModel
    }

    Kirigami.Dialog {
        id: referencesDialog
        property var viewer: null
        parent: root
        modal: true
        title: i18n("References")
        width: Math.min(Kirigami.Units.gridUnit * 40, applicationWindow().width - Kirigami.Units.gridUnit * 4)
        height: Math.min(Kirigami.Units.gridUnit * 35, applicationWindow().height - Kirigami.Units.gridUnit * 4)

        contentItem: Item {
            anchors.fill: parent

            ColumnLayout {
                anchors.fill: parent
                Layout.fillWidth: true
                Layout.fillHeight: true
                anchors.margins: Kirigami.Units.gridUnit
                spacing: Kirigami.Units.smallSpacing

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 2
                    Layout.topMargin: Kirigami.Units.smallSpacing * 2
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                    color: Qt.rgba(1, 1, 1, 0.18)
                    opacity: 0.6
                }

                Kirigami.PlaceholderMessage {
                    Layout.fillWidth: true
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                    visible: referenceOverviewModel.count === 0
                    text: i18n("No references in this book")
                    icon.name: "emblem-symbolic-link"
                }

                ListView {
                    id: referencesListView
                    model: referenceOverviewModel
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: Kirigami.Units.gridUnit * 14
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    visible: referenceOverviewModel.count > 0
                    spacing: Kirigami.Units.smallSpacing
                    clip: true

                    delegate: Delegates.RoundedItemDelegate {
                        id: referenceDelegate
                        width: referencesListView.width
                        property string referenceId: model.ref || ""
                        property string referenceTitle: model.title || ""
                        property string referenceText: model.text || referenceTitle || referenceId
                        property string referenceTooltip: model.tooltip || referenceTitle || referenceText || referenceId
                        property string referenceLocation: model.location || ""
                        property string referenceSection: model.section || ""

                        QQC2.ToolTip.text: referenceTooltip
                        QQC2.ToolTip.visible: hovered && referenceTooltip.length > 0
                        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay

                        onClicked: {
                            if (referenceLocation && referencesDialog.viewer) {
                                referencesDialog.viewer.runJavaScript('reader.view.goTo(' + JSON.stringify(referenceLocation) + ')');
                                referencesDialog.close();
                            }
                        }

                        contentItem: ColumnLayout {
                            spacing: Kirigami.Units.smallSpacing

                            QQC2.Label {
                                Layout.fillWidth: true
                                text: referenceTitle || referenceText || i18n("Reference")
                                wrapMode: Text.WordWrap
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                                maximumLineCount: 2
                                color: Kirigami.Theme.textColor
                            }

                            QQC2.Label {
                                visible: referenceText.length > 0 && referenceText !== referenceTitle
                                Layout.fillWidth: true
                                text: referenceText
                                wrapMode: Text.WordWrap
                                elide: Text.ElideRight
                                maximumLineCount: 2
                                color: Kirigami.Theme.textColor
                            }

                            QQC2.Label {
                                visible: referenceSection.length > 0
                                Layout.fillWidth: true
                                text: referenceSection
                                wrapMode: Text.WordWrap
                                font: Kirigami.Theme.smallFont
                                color: Kirigami.Theme.disabledTextColor
                            }
                        }
                    }
                }
            }
        }
    }

    Kirigami.Dialog {
        id: notesDialog
        property var viewer: null
        parent: root
        modal: true
        title: i18n("Remarks")
        width: Math.min(Kirigami.Units.gridUnit * 40, applicationWindow().width - Kirigami.Units.gridUnit * 4)
        height: Math.min(Kirigami.Units.gridUnit * 35, applicationWindow().height - Kirigami.Units.gridUnit * 4)

        contentItem: Item {
            anchors.fill: parent

            ColumnLayout {
                anchors.fill: parent
                Layout.fillWidth: true
                Layout.fillHeight: true
                anchors.margins: Kirigami.Units.gridUnit
                spacing: Kirigami.Units.smallSpacing

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 2
                    Layout.topMargin: Kirigami.Units.smallSpacing * 2
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                    color: Qt.rgba(1, 1, 1, 0.18)
                    opacity: 0.6
                }

                Kirigami.PlaceholderMessage {
                    id: notesPlaceholder
                    Layout.fillWidth: true
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                    visible: notesModel.count === 0
                    text: i18n("No remarks in this book")
                    icon.name: "comment"
                }

                ListView {
                    id: notesListView
                    model: notesModel
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: Kirigami.Units.gridUnit * 14
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    visible: notesModel.count > 0
                    spacing: Kirigami.Units.smallSpacing
                    clip: true

                    delegate: Delegates.RoundedItemDelegate {
                        id: notesDelegate
                        width: notesListView.width
                        property string annotationKey: model.key || model.annotationId || model.value || ""
                        property string annotationText: model.text || model.value || model.cfiRange || ""
                        property string annotationNote: model.note || ""
                        property string annotationCfi: model.cfi || model.runtimeCfi || model.value || model.cfiRange || ""
                        property string annotationColor: model.color || "#FFD700"

                        onClicked: {
                            if (annotationCfi && notesDialog.viewer) {
                                notesDialog.viewer.runJavaScript('reader.view.goTo("' + annotationCfi + '")');
                                notesDialog.close();
                            }
                        }

                        contentItem: ColumnLayout {
                            spacing: Kirigami.Units.smallSpacing

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Kirigami.Units.smallSpacing

                                Rectangle {
                                    Layout.preferredWidth: Kirigami.Units.gridUnit * 1.0
                                    Layout.preferredHeight: Kirigami.Units.gridUnit * 1.5
                                    color: annotationColor
                                    radius: Kirigami.Units.smallSpacing
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0

                                    QQC2.Label {
                                        Layout.fillWidth: true
                                        text: annotationText || i18n("Highlighted text")
                                        wrapMode: Text.WordWrap
                                        font.weight: Font.Medium
                                        elide: Text.ElideRight
                                        maximumLineCount: 2
                                        color: Kirigami.Theme.textColor
                                    }
                                }

                                QQC2.ToolButton {
                                    Layout.preferredWidth: Kirigami.Units.gridUnit * 2
                                    Layout.preferredHeight: Kirigami.Units.gridUnit * 2
                                    display: QQC2.AbstractButton.IconOnly
                                    icon.name: "edit-delete"
                                    text: i18n("Delete Annotation")
                                    enabled: !root.readOnly && annotationKey.length > 0
                                    QQC2.ToolTip.text: text
                                    QQC2.ToolTip.visible: hovered
                                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                                    onClicked: backend.deleteAnnotation(annotationKey)
                                }
                            }

                            QQC2.Label {
                                visible: annotationNote.length > 0
                                Layout.fillWidth: true
                                text: annotationNote
                                wrapMode: Text.WordWrap
                                font: Kirigami.Theme.smallFont
                                color: Kirigami.Theme.disabledTextColor
                            }
                        }
                    }
                }
            }
        }
    }
    QQC2.ActionGroup {
        id: readerThemeActionGroup
        exclusive: true
    }

    actions: [
        Kirigami.Action {
            text: i18n("References")
            displayHint: Kirigami.DisplayHint.IconOnly
            icon.source: "qrc:/qt/qml/org/kde/arianna/qml/icons/reference-blue.svg"
            onTriggered: {
                referencesDialog.open();
                backend.loadReferenceOverview();
            }
            enabled: referenceOverviewModel.count > 0
        },
        Kirigami.Action {
            text: i18n("Remarks")
            displayHint: Kirigami.DisplayHint.IconOnly
            icon.source: "qrc:/qt/qml/org/kde/arianna/qml/icons/bookmark-gold.svg"
            onTriggered: notesDialog.open()
            enabled: notesModel.count > 0
        },
        Kirigami.Action {
            text: i18nc("@action:intoolbar", "Search")
            displayHint: Kirigami.DisplayHint.IconOnly
            icon.name: "system-search-symbolic"
            onTriggered: searchDialog.open()
        },
        Kirigami.Action {
            text: root.effectivePageMode === "two" ? i18nc("@action:intoolbar", "Switch to One-Page View") : i18nc("@action:intoolbar", "Switch to Two-Page View")
            displayHint: Kirigami.DisplayHint.IconOnly
            icon.name: root.effectivePageMode === "two" ? "view-pages-facing" : "view-pages-single"
            checkable: true
            checked: root.effectivePageMode === "two"
            enabled: view.bookReady
            onTriggered: root.setPageMode(root.effectivePageMode === "two" ? "single" : "two", true)
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
            enabled: root.canEditBookWithContext(false)
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
            enabled: backend.metadata
            onTriggered: {
                applicationWindow().pageStack.pushDialogLayer(Qt.resolvedUrl("./BookDetailsPage.qml"), {
                    metadata: root.detailsMetadata(),
                    bookListModel: applicationWindow().bookListModel,
                    readOnly: root.readOnly,
                    importToLibrary: function(done) {
                        root.importReadOnlyBook(done);
                    }
                });
            }
        }
    ]

    SearchModel {
        id: searchResultModel

        onSearchTriggered: text => {
            if (view.bookReady) {
                view.runJavaScript(`reader.search(${JSON.stringify(text)})`);
            }
        }
    }

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        text: i18n("No book selected")
        visible: root.url === ''
        helpfulAction: Kirigami.Action {
            text: i18n("Open file")
            onTriggered: {
                const fileDialog = openFileDialog.createObject(applicationWindow());
                if (!fileDialog) {
                    console.error("Failed to create openFileDialog");
                    return;
                }
                fileDialog.accepted.connect(() => {
                    const file = fileDialog.file;
                    if (!file) {
                        return;
                    }
                    root.url = file;
                });
                fileDialog.open();
            }
        }
    }

    Component {
        id: openFileDialog

        FileDialog {
            id: openFileDialogObject
            parentWindow: applicationWindow()
            title: i18n("Please choose a file")
            nameFilters: [i18nc("Name filter for EPUB files", "eBook files (*.epub *.cb* *.fb2 *.fb2zip)")]
        }
    }

    Connections {
        target: applicationWindow().contextDrawer
        function onGoTo(cfi) {
            view.goTo(cfi);
        }
    }

    Connections {
        target: Translator
        function onTranslationReady(translatedString) {
            if (!root.translationPending) {
                return;
            }

            root.translationPending = false;
            backend.showTranslation(translatedString);
        }
    }

    Connections {
        target: AnnotationStore
        function onAnnotationsChanged(bookId) {
            const currentBookId = root.bookServerIdentifier();
            if (bookId && bookId !== currentBookId) {
                return;
            }

            backend.loadAnnotations();
        }
    }

    Connections {
        target: ExternalProcess
        function onEditedFileChanged(file) {
            if (file !== root.filename && root.url !== "file://" + file) {
                return;
            }

            reloadChangedBookTimer.restart();
        }
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
            root.handleEditorImportStatus(sessionId, status, message, workingCopyPath, newStateId, anchorValidationIssues);
        }
    }

    Timer {
        id: reloadChangedBookTimer
        interval: 500
        repeat: false
        onTriggered: {
            root.refreshLibraryEntryFromFile();
            root.reloadCurrentBook();
        }
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
                    id: sourceText
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    readOnly: true
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    Kirigami.Dialog {
        id: aiDiscussionDialog
        parent: QQC2.ApplicationWindow.overlay
        modal: true
        title: i18n("AI Discussion")
        standardButtons: QQC2.Dialog.Close
        width: Math.min(Kirigami.Units.gridUnit * 38, parent ? parent.width - Kirigami.Units.gridUnit * 2 : Kirigami.Units.gridUnit * 38)
        height: Math.min(Kirigami.Units.gridUnit * 30, parent ? parent.height - Kirigami.Units.gridUnit * 2 : Kirigami.Units.gridUnit * 30)
        x: parent ? Math.round((parent.width - width) / 2) : 0
        y: parent ? Math.round((parent.height - height) / 2) : 0

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            QQC2.Label {
                id: aiDiscussionSelectedTextLabel
                Layout.fillWidth: true
                text: i18n("Selected text:")
                wrapMode: Text.WordWrap
                font.weight: Font.Medium
            }

            QQC2.Label {
                id: aiDiscussionTocEntryLabel
                Layout.fillWidth: true
                visible: text.length > 0
                wrapMode: Text.WordWrap
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
            }

            QQC2.ScrollView {
                id: aiDiscussionSelectedTextScroll
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 8
                clip: true

                QQC2.TextArea {
                    id: aiDiscussionSelectedText
                    width: aiDiscussionSelectedTextScroll.availableWidth
                    selectByMouse: true
                    wrapMode: Text.Wrap
                    placeholderText: i18n("No selected text available")
                }
            }

            QQC2.Label {
                Layout.fillWidth: true
                text: i18n("Question prompt:")
                font.weight: Font.Medium
            }

            QQC2.ScrollView {
                id: aiDiscussionQuestionPromptScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                QQC2.TextArea {
                    id: aiDiscussionQuestionPrompt
                    width: aiDiscussionQuestionPromptScroll.availableWidth
                    selectByMouse: true
                    wrapMode: Text.Wrap
                    placeholderText: i18n("Enter a question or task for the AI discussion")
                }
            }

            RowLayout {
                Layout.fillWidth: true

                Item {
                    Layout.fillWidth: true
                }

                QQC2.Button {
                    text: i18n("Copy Prompt")
                    icon.name: "edit-copy"
                    enabled: aiDiscussionSelectedText.text.trim().length > 0
                        && aiDiscussionQuestionPrompt.text.trim().length > 0
                    onClicked: backend.copyAiDiscussionPrompt()
                }
            }
        }
    }

    Kirigami.Dialog {
        id: annotationDialog
        parent: QQC2.ApplicationWindow.overlay
        // parent: root
        modal: true
        title: i18nc("@title:dialog", "Annotation")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        width: Math.min(Kirigami.Units.gridUnit * 30, parent ? parent.width - Kirigami.Units.gridUnit * 2 : Kirigami.Units.gridUnit * 30)
        x: parent ? Math.round((parent.width - width) / 2) : 0
        y: parent ? Math.round((parent.height - height) / 2) : 0

        property string annotationKey: ""

        onAccepted: backend.updateAnnotationNote(annotationKey, annotationNote.text)
        onOpened: annotationNote.forceActiveFocus()

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            QQC2.Label {
                id: annotationExcerpt
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                maximumLineCount: 4
                elide: Text.ElideRight
                textFormat: Text.PlainText
            }

            QQC2.TextArea {
                id: annotationNote
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 8
                wrapMode: Text.Wrap
                placeholderText: i18n("Add Note…")
            }
        }
    }

    Kirigami.Dialog {
        id: invalidAnchorImportDialog
        parent: QQC2.ApplicationWindow.overlay
        modal: true
        title: i18nc("@title:dialog", "Import EPUB File")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        width: Math.min(Kirigami.Units.gridUnit * 38, parent ? parent.width - Kirigami.Units.gridUnit * 2 : Kirigami.Units.gridUnit * 38)
        height: Math.min(Kirigami.Units.gridUnit * 34, parent ? parent.height - Kirigami.Units.gridUnit * 2 : Kirigami.Units.gridUnit * 34)
        x: parent ? Math.round((parent.width - width) / 2) : 0
        y: parent ? Math.round((parent.height - height) / 2) : 0

        onAccepted: {
            if (root.pendingInvalidImportSessionId.length > 0) {
                EditorSessionStore.importNow(root.pendingInvalidImportSessionId, true);
            } else if (root.pendingActiveFileImportBookId.length > 0) {
                root.requestActiveFileImport(root.pendingActiveFileImportBookId);
            }
        }

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            QQC2.Label {
                Layout.fillWidth: true
                text: i18n("The EPUB file no longer contains some anchors used by annotations or references. Importing it will keep the data, but Arianna will report the affected entries.")
                wrapMode: Text.WordWrap
            }

            QQC2.Label {
                Layout.fillWidth: true
                visible: root.pendingInvalidImportMessage.length > 0
                text: root.pendingInvalidImportMessage
                wrapMode: Text.WordWrap
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
            }

            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: Kirigami.Units.smallSpacing
                model: root.pendingInvalidImportIssues || []

                delegate: QQC2.ItemDelegate {
                    width: ListView.view.width
                    text: modelData.message || root.validationIssueSummary(modelData)
                    icon.name: modelData.type === "annotation" ? "comment" : "insert-link"
                    hoverEnabled: true
                    QQC2.ToolTip.text: root.validationIssueSummary(modelData)
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }
            }
        }
    }

    FocusScope {
        id: focusScope
        anchors.fill: parent
        focus: true
        focusPolicy: Qt.StrongFocus
        Keys.onPressed: event => {
            let handled = true;
            if (event.modifiers & Qt.ControlModifier) {
                if (event.key === Qt.Key_Left) {
                    view.prevSection();
                } else if (event.key === Qt.Key_Right) {
                    view.nextSection();
                } else if (event.key === Qt.Key_Up) {
                    view.zoomIn();
                } else if (event.key === Qt.Key_Down) {
                    view.zoomOut();
                } else if (event.key === Qt.Key_E) {
                    if (root.filename) {
                        root.editBook();
                    }
                } else {
                    handled = false;
                }
            } else if (event.modifiers & Qt.AltModifier) {
                if (event.key === Qt.Key_Left) {
                    view.goBack();
                } else if (event.key === Qt.Key_Right) {
                    view.goForward();
                } else {
                    handled = false;
                }
            } else {
                if (event.key === Qt.Key_Left) {
                    view.prev();
                } else if (event.key === Qt.Key_Right) {
                    view.next();
                } else {
                    handled = false;
                }
            }
            event.accepted = handled;
        }
    }

    WebEngineView {
        id: view
        anchors.fill: parent
        url: Qt.resolvedUrl("main.html")
        visible: root.url !== '' && !root.readerSuspended
        webChannel: channel
        property bool readerReady: false
        property bool bookReady: false
        property bool readerCanGoBack: false
        property bool readerCanGoForward: false

        settings.javascriptEnabled: true
        settings.localContentCanAccessRemoteUrls: true
        settings.localContentCanAccessFileUrls: true

        Component.onCompleted: {
            view.zoomFactor = root.zoomLevel > 0 ? root.zoomLevel : 1.0;
            referencesDialog.viewer = view;
            notesDialog.viewer = view;
        }

        Component.onDestruction: closeReader()

        function resetReaderState() {
            readerReady = false;
            bookReady = false;
            readerCanGoBack = false;
            readerCanGoForward = false;
        }

        function closeReader() {
            resetReaderState();
            view.runJavaScript(`if (globalThis.closeReader) { globalThis.closeReader(); }`);
        }

        onCertificateError: error => {
            const u = error.url;

            if (u.scheme === "https" && u.host === bookServerAddress && u.port === bookServerPort && error.overridable) {
                console.warn("Accepting local bookserver TLS certificate:", error.description);
                error.acceptCertificate();
                return;
            }

            error.rejectCertificate();
        }
        onVisibleChanged: if (!visible && !root.readerSuspended) {
            closeReader();
            ExternalProcess.clearWatchedFile();
            root.bookClosed();
        }

        onJavaScriptConsoleMessage: (level, message, lineNumber, sourceID) => {
            console.error('WEB:', level, message, lineNumber, sourceID);
        }
        onLoadingChanged: if (loading) {
            resetReaderState();
        }

        onContextMenuRequested: request => {
            if (request.mediaType !== ContextMenuRequest.MediaTypeImage) {
                return;
            }

            request.accepted = true;
            imageContextMenu.popup(view, request.position.x, request.position.y);
        }

        function next() {
            if (bookReady) {
                view.runJavaScript('reader.view.next().catch(error => console.error("reader.view.next failed", error?.stack ?? error))');
            }
        }

        function prev() {
            if (bookReady) {
                view.runJavaScript('reader.view.prev().catch(error => console.error("reader.view.prev failed", error?.stack ?? error))');
            }
        }

        function nextSection() {
            if (bookReady) {
                view.runJavaScript('reader.view.nextSection().catch(error => console.error("reader.view.nextSection failed", error?.stack ?? error))');
            }
        }

        function prevSection() {
            if (bookReady) {
                view.runJavaScript('reader.view.prevSection().catch(error => console.error("reader.view.prevSection failed", error?.stack ?? error))');
            }
        }

        function goTo(cfi) {
            if (bookReady) {
                view.runJavaScript('reader.view.goTo(' + JSON.stringify(cfi) + ').catch(error => console.error("reader.view.goTo failed", error?.stack ?? error))');
            }
        }

        function goBack() {
            if (bookReady && readerCanGoBack) {
                view.runJavaScript('reader.view.history.back()');
            }
        }

        function goForward() {
            if (bookReady && readerCanGoForward) {
                view.runJavaScript('reader.view.history.forward()');
            }
        }

        function setZoomFactor(zoomFactor) {
            const clampedZoomFactor = Math.max(0.5, Math.min(zoomFactor, 3.0));
            if (Math.abs(view.zoomFactor - clampedZoomFactor) < 0.001) {
                return;
            }

            view.zoomFactor = clampedZoomFactor;
            root.zoomLevelSaved(view.zoomFactor);
        }

        function zoomIn() {
            setZoomFactor(view.zoomFactor + 0.1);
        }

        function zoomOut() {
            setZoomFactor(view.zoomFactor - 0.1);
        }

        Connections {
            target: backend
            function onSelectionChanged() {
                Qt.callLater(() => {
                    if (!backend.selection) {
                        return;
                    }

                    if (backend.selection.type === "annotation") {
                        annotationPopup.popup();
                    } else if (backend.selection.type === "reference") {
                        referencePopup.popup();
                    } else {
                        selectionPopup.popup();
                    }
                });
            }
        }

        QQC2.Menu {
            id: selectionPopup

            property bool actionTriggered: false

            function resolveSelection(action) {
                selectionPopup.actionTriggered = true;
                backend.selection = null;
                view.runJavaScript(action === undefined ? "selectionAction()" : "selectionAction(" + JSON.stringify(action) + ")");
            }

            onOpened: actionTriggered = false
            onClosed: {
                if (!actionTriggered) {
                    view.runJavaScript("selectionAction()");
                }
                actionTriggered = false;
                backend.selection = null;
            }

            QQC2.MenuItem {
                text: i18n("Highlight")
                icon.name: 'bookmark-new'
                enabled: backend.canAnnotateSelection()
                onClicked: {
                    if (backend.createAnnotationFromSelection()) {
                        selectionPopup.resolveSelection("highlight");
                    }
                }
            }

            QQC2.MenuItem {
                text: i18n("Create Reference")
                icon.name: 'insert-link'
                enabled: true
                onClicked: {
                    selectionPopup.resolveSelection("reference");
                }
            }

            QQC2.MenuItem {
                text: i18n("AI Discussion")
                icon.name: 'tools-wizard'
                enabled: backend.canDiscussSelectionWithAi()
                onClicked: {
                    if (backend.openAiDiscussionFromSelection()) {
                        selectionPopup.resolveSelection("ai-discussion");
                    }
                }
            }

            QQC2.MenuItem {
                text: i18n("Copy")
                icon.name: 'edit-copy'
                onClicked: selectionPopup.resolveSelection("copy")
            }

            QQC2.MenuItem {
                text: i18n("Find")
                icon.name: 'search'
                onClicked: selectionPopup.resolveSelection("find")
            }

            QQC2.MenuItem {
                text: i18n("Search with Google")
                icon.name: 'internet-web-browser'
                enabled: !!backend.selection && !!backend.selection.text
                onClicked: {
                    if (!backend.selection || !backend.selection.text) {
                        return;
                    }

                    const text = backend.selection.text.trim();
                    if (text.length === 0) {
                        return;
                    }

                    Qt.openUrlExternally("https://www.google.com/search?q=" + encodeURIComponent(text));
                    selectionPopup.resolveSelection("search-google");
                }
            }

            QQC2.MenuItem {
                text: i18n("Edit at Text")
                icon.name: 'document-edit'
                enabled: root.canEditBookWithContext(true) && !!backend.selection && !!backend.selection.text
                onClicked: {
                    if (!backend.selection || !backend.selection.text) {
                        return;
                    }

                    root.editBook(backend.selection.text);
                    selectionPopup.resolveSelection("edit-at-text");
                }
            }
            QQC2.MenuItem {
                text: i18n("Translate")
                icon.name: 'edit-find-replace'
                onClicked: selectionPopup.resolveSelection("translate")
            }
        }

        QQC2.Menu {
            id: annotationPopup

            QQC2.MenuItem {
                text: i18n("Edit Annotation")
                icon.name: 'pencil'
                enabled: backend.selection && (backend.selection.annotationKey || backend.selection.value)
                    && backend.annotationForValue(backend.selection.annotationKey || backend.selection.value) !== null
                onClicked: {
                    backend.openAnnotationEditor(backend.selection.annotationKey || backend.selection.value);
                    view.runJavaScript("selectionAction()");
                }
            }

            QQC2.MenuItem {
                text: i18n("Select Text")
                icon.name: 'edit-select-text'
                onClicked: view.runJavaScript("selectionAction('select')")
            }

            QQC2.MenuItem {
                text: i18n("Delete Annotation")
                icon.name: 'edit-delete'
                enabled: backend.selection && (backend.selection.annotationKey || backend.selection.value)
                    && backend.annotationForValue(backend.selection.annotationKey || backend.selection.value) !== null
                onClicked: backend.deleteAnnotation(backend.selection.annotationKey || backend.selection.value)
            }
        }

        QQC2.Menu {
            id: referencePopup

            property bool actionTriggered: false

            function resolveReference(action) {
                referencePopup.actionTriggered = true;
                backend.selection = null;
                view.runJavaScript(action === undefined ? "selectionAction()" : "selectionAction(" + JSON.stringify(action) + ")");
            }

            onOpened: actionTriggered = false
            onClosed: {
                if (!actionTriggered) {
                    view.runJavaScript("selectionAction()");
                }
                actionTriggered = false;
                backend.selection = null;
            }

            QQC2.MenuItem {
                text: i18n("Open Reference")
                icon.name: 'document-open'
                enabled: backend.selection && backend.selection.canOpen
                onClicked: referencePopup.resolveReference("open-reference")
            }

            QQC2.MenuItem {
                text: i18n("Edit Reference")
                icon.name: 'document-edit'
                enabled: backend.selection && backend.selection.ref
                onClicked: referencePopup.resolveReference("edit-reference")
            }

            QQC2.MenuItem {
                text: i18n("Delete Reference")
                icon.name: 'edit-delete'
                enabled: backend.selection && backend.selection.ref
                onClicked: referencePopup.resolveReference("delete-reference")
            }
        }

        QQC2.Menu {
            id: imageContextMenu

            QQC2.MenuItem {
                text: i18n("Back")
                enabled: view.readerCanGoBack
                onClicked: view.goBack()
            }
            QQC2.MenuItem {
                text: i18n("Forward")
                enabled: view.readerCanGoForward
                onClicked: view.goForward()
            }
            QQC2.MenuItem {
                text: i18n("Reload")
                onClicked: view.triggerWebAction(WebEngineView.Reload)
            }
            QQC2.MenuItem {
                text: i18n("Save page")
                onClicked: view.triggerWebAction(WebEngineView.SavePage)
            }
            QQC2.MenuSeparator {}
            QQC2.MenuItem {
                text: i18n("Set Image not_Inverse")
                enabled: !root.readOnly && root.bookServerIdentifier().length > 0
                onClicked: view.runJavaScript("reader.setContextImageNotInverse()")
            }
            QQC2.MenuSeparator {}
            QQC2.MenuItem {
                text: i18n("Save image")
                onClicked: view.triggerWebAction(WebEngineView.DownloadImageToDisk)
            }
            QQC2.MenuItem {
                text: i18n("Copy image")
                onClicked: view.triggerWebAction(WebEngineView.CopyImageToClipboard)
            }
            QQC2.MenuItem {
                text: i18n("Copy image address")
                onClicked: view.triggerWebAction(WebEngineView.CopyImageUrlToClipboard)
            }
        }
    }

    // Allows the user to move pages/sections by rotating the wheel or zoom with Ctrl+wheel
    MouseArea {
        anchors.fill: view
        acceptedButtons: Qt.BackButton | Qt.ForwardButton
        onClicked: mouse => {
            if (mouse.button === Qt.BackButton) {
                view.goBack();
            } else if (mouse.button === Qt.ForwardButton) {
                view.goForward();
            }
        }
        onWheel: event => {
            if (event.modifiers & Qt.ControlModifier) {
                const dx = event.angleDelta.x;
                const dy = event.angleDelta.y;

                // Adjust page width with horizontal Ctrl-wheel
                if (Math.abs(dx) > Math.abs(dy)) {
                    const step = 25;
                    const newWidth = Config.maxWidth + (dx < 0 ? step : -step);
                    Config.maxWidth = Math.max(100, newWidth);
                    Config.save();
                    backend.applyStyle();
                } else {
                    // Zoom functionality with vertical Ctrl-wheel
                    if (dy > 0) {
                        view.zoomIn();
                    } else if (dy < 0) {
                        view.zoomOut();
                    }
                }
            } else {
                // Navigation functionality
                let dx = event.angleDelta.x;
                let dy = event.angleDelta.y;

                // ignore tiny movements
                if (Math.abs(dx) < 1 && Math.abs(dy) < 1)
                    return;

                if (Math.abs(dx) > Math.abs(dy)) {
                    if (dx > 0)
                        view.prevSection();
                    else if (dx < 0)
                        view.nextSection();
                } else {
                    if (dy > 0)
                        view.prev();
                    else if (dy < 0)
                        view.next();
                }
            }
        }
    }

    footer: QQC2.ToolBar {
        visible: backend.locationsReady
        contentItem: RowLayout {
            QQC2.ToolButton {
                id: progressButton
                text: i18nc("Book reading progress", "%1%", Math.round(backend.progress * 100))
                property bool isMenuOpen: false
                onClicked: {
                    isMenuOpen = !isMenuOpen;
                    if (isMenuOpen) {
                        menu.popup(progressButton, 0, -menu.height);
                    } else {
                        menu.close();
                    }
                }
                Accessible.role: Accessible.ButtonMenu

                property QQC2.Menu menu: QQC2.Menu {
                    width: Kirigami.Units.gridUnit * 10
                    height: Kirigami.Units.gridUnit * 15
                    closePolicy: QQC2.Popup.CloseOnEscape | QQC2.Popup.CloseOnPressOutsideParent
                    contentItem: ColumnLayout {
                        Kirigami.FormLayout {
                            Layout.fillWidth: true
                            QQC2.Label {
                                Kirigami.FormData.label: i18n("Time left in chapter:")
                                text: backend.timeInChapter ? Format.formatDuration(backend.timeInChapter * 1000) : i18n("Loading")
                            }
                            QQC2.Label {
                                Kirigami.FormData.label: i18n("Time left in book:")
                                text: backend.timeInBook ? Format.formatDuration(backend.timeInBook * 1000) : i18n("Loading")
                            }
                        }
                    }
                }
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
            QQC2.ToolButton {
                text: i18n("Back")
                display: QQC2.AbstractButton.IconOnly
                icon.name: "go-previous-view"
                enabled: view.readerCanGoBack
                onClicked: view.goBack()
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
            QQC2.ToolButton {
                text: i18n("Previous Page")
                display: QQC2.AbstractButton.IconOnly
                icon.name: "arrow-left"
                onClicked: view.prev()
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
            QQC2.Slider {
                padding: Kirigami.Units.smallSpacing
                value: backend.progress
                onValueChanged: {
                    if (pressed) {
                        backend.progress = value;
                    }
                }
                onPressedChanged: {
                    if (!pressed) {
                        backend.progress = value;
                        if (view.bookReady) {
                            view.runJavaScript(`reader.view.goToFraction(${value})`);
                        }
                    }
                }
                live: false
                Layout.fillWidth: true
            }
            QQC2.ToolButton {
                text: i18n("Forward")
                display: QQC2.AbstractButton.IconOnly
                icon.name: "go-next-view"
                enabled: view.readerCanGoForward
                onClicked: view.goForward()
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
            QQC2.ToolButton {
                text: i18n("Next Page")
                icon.name: "arrow-right"
                onClicked: view.next()
                display: QQC2.AbstractButton.IconOnly
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }
    }

    QtObject {
        id: backend
        WebChannel.id: "backend"
        property var selection: null
        property var annotationMap: ({})
        property var annotations: []
        property var referenceOverview: []
        property double progress: 0
        property var location
        property var locations: root.locations
        property bool locationsReady: false
        property var metadata: null
        property var top: ({})
        property string file: root.url
        property int timeInChapter: 0
        property int timeInBook: 0

        function get(script, callback) {
            return view.runJavaScript(`JSON.stringify(${script})`, callback);
        }
        function showTranslation(text) {
            sourceText.text = text;
            translatorDialog.open();
        }
        function promptTextValue(value) {
            if (value === undefined || value === null) {
                return "";
            }
            if (Array.isArray(value)) {
                return value.map(item => promptTextValue(item)).filter(item => item.length > 0).join(", ");
            }
            if (typeof value === "object") {
                if (value.name) {
                    return promptTextValue(value.name);
                }
                if (value.label) {
                    return promptTextValue(value.label);
                }
                if (value.value) {
                    return promptTextValue(value.value);
                }
                if (value.text) {
                    return promptTextValue(value.text);
                }
                return "";
            }
            return String(value).trim();
        }
        function selectedDiscussionText() {
            if (!selection) {
                return "";
            }

            return promptTextValue(selection.content || selection.text || "");
        }
        function canDiscussSelectionWithAi() {
            return selectedDiscussionText().length > 0;
        }
        function bookTitleContextForPrompt() {
            return promptTextValue((metadata ? metadata.title : "") || (root.entry ? root.entry.title : "") || root.filename);
        }
        function bookAuthorContextForPrompt() {
            return promptTextValue((root.entry ? root.entry.author : "") || (metadata ? metadata.creator : "") || (metadata ? metadata.author : ""));
        }
        function bookTitleForPrompt() {
            return bookTitleContextForPrompt() || i18n("Unknown");
        }
        function bookAuthorForPrompt() {
            return bookAuthorContextForPrompt() || i18n("Unknown");
        }
        function tocEntryForPrompt() {
            const selectionTocItem = selection ? selection.tocItem : null;
            const locationTocItem = location ? location.tocItem : null;
            return promptTextValue((selectionTocItem && selectionTocItem.label ? selectionTocItem.label : selectionTocItem)
                || (locationTocItem && locationTocItem.label ? locationTocItem.label : locationTocItem));
        }
        function aiDiscussionTocEntryText() {
            const tocEntry = tocEntryForPrompt();
            return tocEntry.length > 0 ? i18n("Table of contents entry: %1", tocEntry) : "";
        }
        function aiDiscussionSelectedTextHeading() {
            const title = bookTitleContextForPrompt();
            const author = bookAuthorContextForPrompt();
            if (title.length > 0 && author.length > 0) {
                return i18n("Selected text from %1 by %2:", title, author);
            }
            if (title.length > 0) {
                return i18n("Selected text from %1:", title);
            }
            if (author.length > 0) {
                return i18n("Selected text by %1:", author);
            }
            return i18n("Selected text:");
        }
        function defaultAiDiscussionQuestionPrompt() {
            return [
                i18n("- Discuss the central claim of the passage."),
                i18n("- Explain important terms, allusions, or assumptions."),
                i18n("- Ask critical questions that help deepen the reading."),
                i18n("- Mention possible cross-references to similar passages when useful.")
            ].join("\n");
        }
        function buildAiDiscussionPrompt(selectedText, questionPrompt) {
            const discussionText = promptTextValue(selectedText === undefined ? selectedDiscussionText() : selectedText);
            const discussionPrompt = promptTextValue(questionPrompt === undefined ? defaultAiDiscussionQuestionPrompt() : questionPrompt);
            if (!discussionText || !discussionPrompt) {
                return "";
            }

            const location = promptTextValue(selection.value || root.currentLocation || "");
            const language = promptTextValue(selection.lang || "");
            const tocEntry = tocEntryForPrompt();
            const lines = [
                i18n("You are a careful discussion partner for literary, philosophical, and factual texts."),
                "",
                i18n("Context:"),
                i18n("Book: %1", bookTitleForPrompt()),
                i18n("Author: %1", bookAuthorForPrompt()),
            ];
            if (tocEntry.length > 0) {
                lines.push(i18n("Table of contents entry: %1", tocEntry));
            }
            if (location.length > 0) {
                lines.push(i18n("Location: %1", location));
            }
            if (language.length > 0) {
                lines.push(i18n("Selection language: %1", language));
            }
            lines.push(
                "",
                i18n("Selected text:"),
                "\"\"\"",
                discussionText,
                "\"\"\"",
                "",
                i18n("Task:"),
                discussionPrompt
            );
            return lines.join("\n");
        }
        function openAiDiscussionFromSelection() {
            const selectedText = selectedDiscussionText();
            if (!selectedText) {
                return false;
            }

            aiDiscussionSelectedText.text = selectedText;
            aiDiscussionSelectedTextLabel.text = aiDiscussionSelectedTextHeading();
            aiDiscussionTocEntryLabel.text = aiDiscussionTocEntryText();
            aiDiscussionQuestionPrompt.text = defaultAiDiscussionQuestionPrompt();
            aiDiscussionDialog.open();
            aiDiscussionQuestionPrompt.forceActiveFocus();
            return true;
        }
        function copyAiDiscussionPrompt() {
            const prompt = buildAiDiscussionPrompt(aiDiscussionSelectedText.text, aiDiscussionQuestionPrompt.text);
            if (prompt.length > 0) {
                Clipboard.saveText(prompt);
            }
        }
        function openReferencePage(location, entry, readOnly, sourceTitle) {
            if (!location) {
                console.warn('Cannot open reference page, missing location');
                return;
            }

            const referenceEntry = entry && entry.filename ? entry : root.entry;
            const referenceFilename = referenceEntry && referenceEntry.filename ? referenceEntry.filename : root.filename;
            const referenceLocations = entry && entry.locations ? entry.locations : root.locations;
            const referenceReadOnly = readOnly === true || root.readOnly;

            if (!referenceFilename) {
                console.warn('Cannot open reference page, missing filename');
                return;
            }

            if (typeof applicationWindow().openReferencePage === 'function') {
                applicationWindow().openReferencePage(referenceFilename, location, referenceEntry, referenceReadOnly, referenceLocations, sourceTitle);
                return;
            }

            applicationWindow().pageStack.layers.push('./EpubViewerPage.qml', {
                currentLocation: location,
                locations: referenceLocations,
                zoomLevel: referenceEntry && referenceEntry.zoomLevel !== undefined && referenceEntry.zoomLevel !== null ? referenceEntry.zoomLevel : root.zoomLevel,
                pageMode: referenceEntry && referenceEntry.pageMode ? referenceEntry.pageMode : root.pageMode,
                filename: referenceFilename,
                entry: referenceEntry,
                readOnly: referenceReadOnly,
                referenceSourceTitle: sourceTitle,
                url: root.url
            });
        }
        function cloneAnnotation(annotation) {
            const copy = {};
            if (!annotation) {
                return copy;
            }

            for (const key in annotation) {
                copy[key] = annotation[key] === undefined || annotation[key] === null ? "" : annotation[key];
            }

            if (!copy.cfiRange && copy.value) {
                copy.cfiRange = copy.value;
            }
            if (!copy.value && copy.cfiRange) {
                copy.value = copy.cfiRange;
            }

            return copy;
        }
        function annotationKey(annotation) {
            if (!annotation) {
                return "";
            }

            return annotation.annotationId || annotation.anchorId || annotation.cfiRange || annotation.value || "";
        }
        function annotationCfi(annotation) {
            if (!annotation) {
                return "";
            }

            return annotation.runtimeCfi || annotation.value || annotation.cfiRange || annotation.cfi || "";
        }
        function annotationList() {
            return annotations ? annotations.slice() : [];
        }
        function updateNotesModel() {
            notesModel.clear();
            const list = annotationList();
            for (let i = 0; i < list.length; ++i) {
                const annotation = list[i];
                const key = annotationKey(annotation);
                const cfi = annotationCfi(annotation);
                notesModel.append({
                    key: key,
                    annotationId: annotation.annotationId || "",
                    anchorId: annotation.anchorId || "",
                    invalid: annotation.invalid || false,
                    value: cfi,
                    cfiRange: annotation.cfiRange || "",
                    runtimeCfi: annotation.runtimeCfi || "",
                    text: annotation.text || cfi || "",
                    color: annotation.color || "#FFD700",
                    note: annotation.note || "",
                    cfi: cfi,
                    created: annotation.created || "",
                    modified: annotation.modified || ""
                });
            }
        }
        function updateReferenceOverviewModel(references) {
            referenceOverviewModel.clear();
            const list = references || [];
            for (let i = 0; i < list.length; ++i) {
                const reference = list[i];
                referenceOverviewModel.append({
                    ref: reference.ref || "",
                    title: reference.title || "",
                    text: reference.text || "",
                    tooltip: reference.tooltip || "",
                    location: reference.location || "",
                    section: reference.section || ""
                });
            }
        }
        function loadReferenceOverview() {
            if (!view.bookReady) {
                return;
            }

            backend.referenceOverview = [];
            backend.updateReferenceOverviewModel(backend.referenceOverview);
            view.runJavaScript("reader.refreshReferenceOverview()");
        }
        function cloneAnnotationsMap() {
            const copy = {};
            for (const key in annotationMap) {
                copy[key] = annotationMap[key];
            }
            return copy;
        }
        function annotationForValue(value) {
            if (!value || !annotationMap[value]) {
                const list = annotationList();
                for (let i = 0; i < list.length; ++i) {
                    const annotation = list[i];
                    if (annotationCfi(annotation) === value || annotation.cfiRange === value || annotation.value === value) {
                        return annotation;
                    }
                }
                return null;
            }

            return annotationMap[value];
        }
        function addAnnotationToView(annotation) {
            if (!view.bookReady || !annotation || annotation.invalid || (!annotation.anchorId && !annotation.value && !annotation.cfiRange)) {
                return;
            }

            view.runJavaScript(`reader.addAnnotation(${JSON.stringify(annotation)})`);
        }
        function removeAnnotationFromView(annotation) {
            if (!view.bookReady || !annotation || (!annotation.anchorId && !annotation.value && !annotation.cfiRange)) {
                return;
            }

            view.runJavaScript(`reader.deleteAnnotation(${JSON.stringify(annotation)})`);
        }
        function renderAnnotations() {
            const list = annotationList();
            for (let i = 0; i < list.length; ++i) {
                addAnnotationToView(list[i]);
            }
        }
        function loadAnnotations() {
            const previousAnnotations = annotationMap || {};
            const nextAnnotations = {};
            const nextAnnotationsList = [];
            if (root.entry.uniqueIdentifier.length > 0) {
                const loadedAnnotations = AnnotationStore.loadAnnotations(root.entry.uniqueIdentifier);
                for (let i = 0; i < loadedAnnotations.length; ++i) {
                    const annotation = loadedAnnotations[i];
                    if (annotation.anchorId || annotation.cfiRange || annotation.value) {
                        const copy = cloneAnnotation(annotation);
                        const key = annotationKey(copy);
                        if (!key) {
                            continue;
                        }
                        nextAnnotations[key] = copy;
                        nextAnnotationsList.push(copy);
                    }
                }
            }

            for (const key in previousAnnotations) {
                if (!nextAnnotations[key]) {
                    removeAnnotationFromView(previousAnnotations[key]);
                }
            }

            annotationMap = nextAnnotations;
            annotations = nextAnnotationsList;
            updateNotesModel();
            renderAnnotations();
        }
        function canAnnotateSelection() {
            return !root.readOnly && root.entry.uniqueIdentifier.length > 0 && selection && selection.type === "selection" && selection.value;
        }
        function rememberAnnotation(annotation) {
            if (!annotation || (!annotation.anchorId && !annotation.value && !annotation.cfiRange)) {
                return false;
            }

            const copy = cloneAnnotation(annotation);
            const key = annotationKey(copy);
            if (!key) {
                return false;
            }
            const nextAnnotations = cloneAnnotationsMap();
            nextAnnotations[key] = copy;

            const nextAnnotationsList = annotationList();
            const existingIndex = nextAnnotationsList.findIndex(item => annotationKey(item) === key);
            if (existingIndex !== -1) {
                nextAnnotationsList[existingIndex] = copy;
            } else {
                nextAnnotationsList.push(copy);
            }

            annotationMap = nextAnnotations;
            annotations = nextAnnotationsList;
            updateNotesModel();
            addAnnotationToView(copy);
            return true;
        }
        function saveAnnotation(annotation) {
            if (!annotation || (!annotation.anchorId && !annotation.value && !annotation.cfiRange) || root.readOnly || root.entry.uniqueIdentifier.length === 0) {
                return false;
            }

            const copy = cloneAnnotation(annotation);
            AnnotationStore.saveAnnotation(root.entry.uniqueIdentifier, copy);
            return rememberAnnotation(copy);
        }
        function createAnnotationFromSelection() {
            if (!canAnnotateSelection()) {
                return null;
            }

            const existingAnnotation = annotationForValue(selection.value);
            if (existingAnnotation) {
                return existingAnnotation;
            }

            const annotation = {
                value: selection.value,
                cfiRange: selection.value,
                color: "yellow",
                text: selection.content || selection.text || "",
                note: "",
                created: new Date().toISOString(),
                modified: ""
            };
            const expectedStateId = AnnotationStore.currentStateId(root.entry.uniqueIdentifier);
            const anchoredAnnotation = AnnotationStore.createAnchoredAnnotation(root.entry.uniqueIdentifier, annotation, expectedStateId);
            if (!anchoredAnnotation || anchoredAnnotation.error) {
                console.warn("Unable to create anchored annotation:", anchoredAnnotation ? anchoredAnnotation.message || "" : "");
                return null;
            }

            return rememberAnnotation(anchoredAnnotation) ? anchoredAnnotation : null;
        }
        function updateAnnotationNote(key, note) {
            const annotation = annotationForValue(key);
            if (!annotation) {
                return;
            }

            const copy = cloneAnnotation(annotation);
            copy.note = note;
            copy.modified = new Date().toISOString();
            saveAnnotation(copy);
        }
        function deleteAnnotation(key) {
            const annotation = annotationForValue(key);
            if (!annotation || root.readOnly || root.entry.uniqueIdentifier.length === 0) {
                return;
            }

            removeAnnotationFromView(annotation);
            AnnotationStore.removeAnnotation(root.entry.uniqueIdentifier, annotationKey(annotation));
            const nextAnnotations = cloneAnnotationsMap();
            delete nextAnnotations[annotationKey(annotation)];
            annotationMap = nextAnnotations;
            annotations = annotations.filter(item => annotationKey(item) !== annotationKey(annotation));
            updateNotesModel();
        }
        function openAnnotationEditor(key) {
            const annotation = annotationForValue(key);
            if (!annotation) {
                return;
            }

            annotationDialog.annotationKey = annotationKey(annotation);
            annotationExcerpt.text = annotation.text || "";
            annotationNote.text = annotation.note || "";
            annotationDialog.open();
        }
        function updateAnnotationRuntimeLocation(location) {
            if (!location) {
                return;
            }

            const key = location.annotationId || location.key || location.anchorId || "";
            const value = location.value || location.runtimeCfi || "";
            if (!key || !value || !annotationMap[key]) {
                return;
            }

            const copy = cloneAnnotation(annotationMap[key]);
            copy.value = value;
            copy.runtimeCfi = value;
            if (!copy.text && location.text) {
                copy.text = location.text;
            }

            const nextAnnotations = cloneAnnotationsMap();
            nextAnnotations[key] = copy;
            const nextAnnotationsList = annotationList();
            const existingIndex = nextAnnotationsList.findIndex(item => annotationKey(item) === key);
            if (existingIndex !== -1) {
                nextAnnotationsList[existingIndex] = copy;
            }

            annotationMap = nextAnnotations;
            annotations = nextAnnotationsList;
            updateNotesModel();
        }
        function dispatch(action) {
            switch (action.type) {
            case 'ready':
                const uiText = {
                    loc: i18n("Loc. %s of %s"),
                    page: i18n("Page %s of %s"),
                    pageWithoutTotal: i18n("Page %s"),
                    close: i18n("Close"),
                    referenceDialog: {
                        saveChangeOfReference: i18n("save change of reference")
                    },
                    references: {
                        "footnote": i18n("Footnote"),
                        "footnote-go": i18n("Go to Footnote"),
                        "endnote": i18n("Endnote"),
                        "endnote-go": i18n("Go to Endnote"),
                        "note": i18n("Note"),
                        "note-go": i18n("Go to Note"),
                        "glossary": i18n("Definition"),
                        "glossary-go": i18n("Go to Definition"),
                        "biblioentry": i18n("Bibliography"),
                        "biblioentry-go": i18n("Go to Bibliography")
                    }
                };

                view.readerReady = true;
                view.runJavaScript(`init({'uiText': ${JSON.stringify(uiText)}})`);
                root.reloadBook();
                break;
            case 'book-ready':
                view.bookReady = true;
                searchResultModel.clear();
                searchResultModel.loading = false;

                const {
                    book
                } = action.payload;
                applicationWindow().contextDrawer.clearCurrentItem();
                if (book && book.toc) {
                    applicationWindow().contextDrawer.model.importFromJson(JSON.stringify(book.toc));
                } else {
                    applicationWindow().contextDrawer.model.importFromJson("[]");
                    console.warn('Book or TOC not available');
                }

                applyStyle();
                backend.loadAnnotations();
                backend.loadReferenceOverview();

                const metadata = action.payload.book.metadata;
                if (metadata) {
                    backend.metadata = metadata;
                    root.applyUpdatedBookEntry(root.entry);
                    root.bookReady(backend.metadata.title);
                } else {
                    view.next();
                }
                break;
            case 'book-error':
                if (action.payload && action.payload.status === "anchor-validation-failed") {
                    root.pendingInvalidImportSessionId = "";
                    root.pendingActiveFileImportBookId = action.payload.bookId || root.bookServerIdentifier();
                    root.pendingInvalidImportWorkingCopyPath = "";
                    root.pendingInvalidImportMessage = action.payload.message || "";
                    root.pendingInvalidImportIssues = action.payload.anchorValidationIssues || [];
                    invalidAnchorImportDialog.open();
                    break;
                }
                console.error('Book error', action.payload);
                break;
            case 'selection':
                if (action.payload.action === 'copy') {
                    Clipboard.saveText(action.payload.text);
                } else if (action.payload.action === 'translate') {
                    root.translationPending = true;
                    backend.showTranslation(i18n("Translating..."));
                    Translator.translate(action.payload.text);
                } else {
                    backend.selection = action.payload;
                }
                break;
            case 'selection-find':
                searchDialog.text = action.payload.text;
                searchResultModel.search(action.payload.text);
                searchResultModel.loading = true;
                searchDialog.open();
                break;
            case 'manual-anchor-required':
                console.warn("Manual anchor required", action.payload.anchorOpenTag, action.payload.anchorCloseTag, action.payload);
                if (!root.editBook(action.payload.text || "",
                                   action.payload.cfi || "",
                                   action.payload.sourceAnchorId || "",
                                   action.payload.anchorOpenTag || "",
                                   action.payload.anchorCloseTag || "")) {
                    console.warn("Manual anchor creation requires a configured editor", action.payload);
                }
                break;
            case 'book-reload-needed':
                root.reloadCurrentBookIfModified(action.payload);
                break;
            case 'create-overlay':
                backend.renderAnnotations();
                break;
            case 'annotation-location':
                if (action.payload.bookId && action.payload.bookId !== root.bookServerIdentifier()) {
                    break;
                }
                backend.updateAnnotationRuntimeLocation(action.payload);
                break;
            case 'show-selection':
                backend.selection = action.payload;
                break;
            case 'reference-overview':
                if (action.payload.bookId && action.payload.bookId !== root.bookServerIdentifier()) {
                    break;
                }
                backend.referenceOverview = action.payload.references || [];
                backend.updateReferenceOverviewModel(backend.referenceOverview);
                break;
            case 'relocate':
                backend.progress = action.payload.fraction;
                backend.location = action.payload;
                root.currentLocation = action.payload.cfi;
                root.currentProgress = action.payload.fraction * 100;
                root.updateCurrentTableOfContentsItem(action.payload.tocItem);
                backend.timeInChapter = Math.round(action.payload.time.section * 60);
                backend.timeInBook = Math.round(action.payload.time.total * 60);
                root.relocated(action.payload.cfi, action.payload.fraction * 100);
                backend.locationsReady = true;
                break;
            case 'history-index-change':
                view.readerCanGoBack = action.payload.canGoBack;
                view.readerCanGoForward = action.payload.canGoForward;
                break;
            case 'find-results':
                searchResultModel.resultFound(action.payload.query, action.payload.results);
                break;
            case 'external-link':
                Qt.openUrlExternally(action.payload.href);
                break;
            }
        }

        function applyStyle() {
            if (!view.bookReady) {
                return;
            }

            // Use the enum value directly; derived bindings can still contain the
            // previous value while this change handler is running.
            const readerThemeInverted = root.currentReaderTheme === 1;
            const readerThemeUsesSystemColors = root.currentReaderTheme === 2;

            const getIbooksInternalTheme = bgColor => {
                const red = bgColor.r;
                const green = bgColor.g;
                const blue = bgColor.b;
                const l = 0.299 * red + 0.587 * green + 0.114 * blue;
                if (l < 0.3)
                    return 'Night';
                else if (l < 0.7)
                    return 'Gray';
                else if (red > green && green > blue)
                    return 'Sepia';
                else
                    return 'White';
            };
            const fontDesc = Config.defaultFont;
            const fontFamily = fontDesc.family;
            const fontSizePt = fontDesc.pointSize;
            const fontSize = fontDesc.pixelSize;
            let fontWeight = 400;
            const fontStyle = fontDesc.styleName;
            const defaultTheme = readerThemeInverted ? {
                light: {
                    fg: "#ffffff",
                    bg: "#000000",
                    link: "#8ab4f8"
                },
                dark: {
                    fg: "#ffffff",
                    bg: "#000000",
                    link: "#8ab4f8"
                },
                inverted: {
                    fg: "#ffffff",
                    bg: "#000000",
                    link: "#8ab4f8"
                }
            } : {
                light: {
                    fg: "#000000",
                    bg: "#ffffff",
                    link: "#0000ee"
                },
                dark: {
                    fg: "#000000",
                    bg: "#ffffff",
                    link: "#0000ee"
                },
                inverted: {
                    fg: "#ffffff",
                    bg: "#000000",
                    link: "#8ab4f8"
                }
            };
            const kdeForegroundColor = readerThemeInverted ? Kirigami.Theme.backgroundColor.toString() : Kirigami.Theme.textColor.toString();
            const kdeBackgroundColor = readerThemeInverted ? Kirigami.Theme.textColor.toString() : Kirigami.Theme.backgroundColor.toString();
            const kdeLinkColor = Kirigami.Theme.highlightColor.toString();
            const kdeTheme = {
                light: {
                    fg: kdeForegroundColor,
                    bg: kdeBackgroundColor,
                    link: kdeLinkColor
                },
                dark: {
                    fg: kdeForegroundColor,
                    bg: kdeBackgroundColor,
                    link: kdeLinkColor
                },
                inverted: {
                    fg: kdeForegroundColor,
                    bg: kdeBackgroundColor,
                    link: kdeLinkColor
                }
            };
            const invertStylesheet = `
                html, body {
                    color: #ffffff !important;
                    background: #000000 !important;
                }
                *, *::before, *::after {
                    color: inherit !important;
                    border-color: currentColor !important;
                    background-color: transparent !important;
                }
                a:any-link {
                    color: #8ab4f8 !important;
                }
                svg, img {
                    background-color: transparent !important;
                    filter: invert(1) hue-rotate(180deg) !important;
                }
                svg.not_inverse,
                img.not_inverse,
                .not_inverse svg,
                .not_inverse img {
                    filter: none !important;
                }
                mjx-container,
                mjx-container *,
                mjx-container svg,
                mjx-container svg * {
                    color: #ffffff !important;
                    background: transparent !important;
                    background-color: transparent !important;
                    border-color: currentColor !important;
                    stroke: currentColor !important;
                }
                mjx-container svg {
                    filter: none !important;
                    fill: currentColor !important;
                }
                mjx-container svg [fill="none"] {
                    fill: none !important;
                }
                mjx-container svg [stroke] {
                    stroke: currentColor !important;
                }
            `;

            const transparentStylesheet = `
                html, body {
                    background: transparent !important;
                    background-color: transparent !important;
                }
                body * {
                    background: transparent !important;
                    background-color: transparent !important;
                    background-image: none !important;
                }
            `;

            const hasReaderBackground = Config.readerBackgroundPath.length > 0;
            const readerBackgroundOverlay = readerThemeInverted ? 'rgba(0, 0, 0, 0.86)' : readerThemeUsesSystemColors ? kdeBackgroundColor : 'rgba(255, 255, 255, 0.86)';
            const readerBackgroundImage = hasReaderBackground ? `linear-gradient(${readerBackgroundOverlay}, ${readerBackgroundOverlay}), url("${bookServerBaseUrl}/static/background-image")` : '';
            const readerBackgroundFilter = 'none';
            const invertBackgroundStylesheet = hasReaderBackground ? `
                html, body {
                    color: #ffffff !important;
                    background: transparent !important;
                    background-color: transparent !important;
                }
                *, *::before, *::after {
                    color: inherit !important;
                    border-color: currentColor !important;
                    background-color: transparent !important;
                }
                body * {
                    background: transparent !important;
                    background-color: transparent !important;
                    background-image: none !important;
                }
                a:any-link {
                    color: #8ab4f8 !important;
                }
                svg, img {
                    background-color: transparent !important;
                    filter: invert(1) hue-rotate(180deg) !important;
                }
                svg.not_inverse,
                img.not_inverse,
                .not_inverse svg,
                .not_inverse img {
                    filter: none !important;
                }
                mjx-container,
                mjx-container *,
                mjx-container svg,
                mjx-container svg * {
                    color: #ffffff !important;
                    background: transparent !important;
                    background-color: transparent !important;
                    border-color: currentColor !important;
                    stroke: currentColor !important;
                }
                mjx-container svg {
                    filter: none !important;
                    fill: currentColor !important;
                }
                mjx-container svg [fill="none"] {
                    fill: none !important;
                }
                mjx-container svg [stroke] {
                    stroke: currentColor !important;
                }
            ` : invertStylesheet;
            const referenceStylesheet = `
                .bookref,
                a[data-role="anchor"][data-anchor-type="crossref"],
                a[data-role="anchor"][href]:not([data-anchor-type="crossref"]) {
                    border-bottom: 1px dashed currentColor;
                    color: inherit;
                    text-decoration: none;
                    cursor: pointer;
                }
                .bookref:hover,
                a[data-role="anchor"][data-anchor-type="crossref"]:hover,
                a[data-role="anchor"][href]:not([data-anchor-type="crossref"]):hover {
                    border-bottom-style: solid;
                }
                .bookref::before,
                a[data-role="anchor"][data-anchor-type="crossref"]::before,
                a[data-role="anchor"][href]:not([data-anchor-type="crossref"])::before {
                    content: "";
                    display: inline-block;
                    width: 1em;
                    height: 1em;
                    margin-right: 0.2em;
                    vertical-align: text-bottom;
                    background: currentColor !important;
                    -webkit-mask: url("${bookServerBaseUrl}/static/book-icon")
                            no-repeat center;
                    -webkit-mask-size: contain;
                        mask: url("${bookServerBaseUrl}/static/book-icon")
                            no-repeat center;
                        mask-size: contain;
                }
                .bookref:hover::before,
                a[data-role="anchor"][data-anchor-type="crossref"]:hover::before,
                a[data-role="anchor"][href]:not([data-anchor-type="crossref"]):hover::before {
                    opacity: 0.8;
                }
`;

            const contentStylesheet = readerThemeInverted ? invertBackgroundStylesheet : (hasReaderBackground || !readerThemeUsesSystemColors ? transparentStylesheet : '');

            const style = {
                layout: root.currentReaderLayout(),
                style: {
                    lineHeight: 1.5,
                    justify: Config.justify,
                    hyphenate: Config.hyphenate,
                    invert: readerThemeInverted,
                    theme: readerThemeUsesSystemColors ? kdeTheme : defaultTheme,
                    overrideFont: !Config.usePublisherFont,
                    userStylesheet: contentStylesheet + referenceStylesheet,
                    searchResultColor: root.searchResultOutlineColor,
                    readerBackgroundImage: readerBackgroundImage,
                    readerBackgroundFilter: readerBackgroundFilter
                }
            };

            view.runJavaScript(`reader.setAppearance(${JSON.stringify(style)})`);
        }
    }

    WebChannel {
        id: channel
        registeredObjects: [backend]
    }

    Shortcut {
        sequence: "Right"
        onActivated: view.next()
    }

    Shortcut {
        sequence: "Left"
        onActivated: view.prev()
    }

    Shortcut {
        sequence: "SHIFT+Right"
        onActivated: view.next()
    }

    Shortcut {
        sequence: "SHIFT+Left"
        onActivated: view.prev()
    }

    Shortcut {
        sequence: "Alt+Left"
        onActivated: view.goBack()
    }

    Shortcut {
        sequence: "Alt+Right"
        onActivated: view.goForward()
    }

    Shortcut {
        sequence: "Ctrl+Left"
        onActivated: view.prevSection()
    }

    Shortcut {
        sequence: "Ctrl+Right"
        onActivated: view.nextSection()
    }

    Shortcut {
        sequence: "Ctrl+Up"
        onActivated: view.zoomIn()
    }

    Shortcut {
        sequence: "Ctrl+Down"
        onActivated: view.zoomOut()
    }

    Shortcut {
        sequence: "Ctrl+E"
        enabled: root.canEditBookWithContext(false)
        onActivated: root.editBook()
    }

    Shortcut {
        sequence: "Ctrl+R"
        enabled: !view.loading
        onActivated: root.reloadCurrentBook()
    }

    Shortcut {
        sequence: "Ctrl+T"
        onActivated: root.cycleReaderThemeMode()
    }

    Shortcut {
        sequence: "Ctrl+B"
        onActivated: ExternalProcess.startDetached(applicationFilePath)
    }

    Shortcut {
        sequence: "Ctrl+S"
        onActivated: applicationWindow().toggleReaderFullScreen()
    }
}
