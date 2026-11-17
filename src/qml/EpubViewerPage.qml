// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQml.Models
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
    property int pageWidthDistance: root.entry && root.entry.pageWidthDistance >= 0 ? root.entry.pageWidthDistance : -1
    readonly property int effectivePageWidth: root.pageWidthDistance >= 100 ? root.pageWidthDistance : (Config.maxWidth || 720)
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
    property bool noLibraryImport: false
    readonly property bool libraryManaged: !readOnly && !noLibraryImport
    property bool readerSuspended: false
    property bool savingRichRemark: false
    property int translationRequestId: 0
    property string searchText: ""
    property var initialSearch: null
    property var speechSelection: []
    property bool continuousSpeech: false
    // Once -> repeat the captured selection -> continue through the document.
    property int speechMode: 0
    property bool finiteSpeechSentence: false
    readonly property var mathJaxRenderingOptions: ({scale: Config.mathJaxScale / 100,
        inlineScale: Config.mathJaxInlineScale / 100,
        align: Config.mathJaxAlignment, overflow: Config.mathJaxOverflow,
        inline: Config.mathJaxBreakInline, fontCache: Config.mathJaxFontCache})
    onMathJaxRenderingOptionsChanged: if (Config.mathJaxSupport && view?.readerReady && root.url) mathSettingsReload.restart()
    property int speechStreamId: 0
    readonly property string mathSpeechLocale: speech.voices.find(voice => voice.id === Config.speechVoice)?.language || ""
    onMathSpeechLocaleChanged: syncMathSpeechOptions()

    function syncMathSpeechOptions() {
        if (view.readerReady) {
            const options = JSON.stringify({locale: root.mathSpeechLocale, concise: true});
            view.runJavaScript("globalThis.setMathSpeechOptions?.(" + options + "); globalThis.setSpeechOptions?.(" + options + ")");
        }
    }

    Connections {
        target: Config
        function onSpeechVoiceChanged() {
            if (view.readerReady && Config.speechVoice) speech.refreshVoices();
            root.syncMathSpeechOptions();
        }
    }

    function cancelContinuousSpeech() {
        continuousSpeech = false;
        speechStreamId++;
        view.runJavaScript("globalThis.reader?.stopSpeechStream()");
    }

    function readSpeechSelection(segments) {
        if (speechMode === 2 && segments.length && segments[0].cfi) {
            readDocumentFrom(segments[0].cfi);
            return;
        }
        cancelContinuousSpeech();
        speechControls.open();
        speech.speakSegments(segments);
    }

    function readDocumentFrom(cfi) {
        speech.stop();
        cancelContinuousSpeech();
        continuousSpeech = true;
        speechMode = 2;
        speech.repeatSelection = false;
        speechControls.open();
        view.runJavaScript("globalThis.reader?.startSpeechStream(" + JSON.stringify(speechStreamId) + ", " + JSON.stringify(cfi) + ")");
    }

    function setSpeechMode(mode) {
        const wasContinuous = continuousSpeech;
        speechMode = mode;
        speech.repeatSelection = mode === 1;
        if (wasContinuous && mode !== 2) {
            cancelContinuousSpeech();
            finiteSpeechSentence = true;
            speech.finishSentence(mode === 1);
        } else if (finiteSpeechSentence && mode === 1) {
            speech.finishSentence(true);
        } else if (!wasContinuous && mode === 2 && speech.state !== SpeechController.Idle) {
            continuousSpeech = true;
            speechStreamId++;
            speech.extendReading(String(speechStreamId));
        }
    }
    property var headerSearchField: null
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
    signal pageWidthDistanceSaved(value: int)
    signal bookReady(title: var)
    signal bookClosed
    signal addToLibraryRequested(var callback)

    onCurrentReaderThemeChanged: backend.applyStyle()
    onEffectivePageModeChanged: backend.applyStyle()
    onEffectivePageWidthChanged: backend.applyStyle()

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
        const maxWidth = root.effectivePageWidth;
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

    function changePageWidth(delta, source) {
        const applyWidth = maximum => {
            const limit = Number(maximum);
            const bounded = isFinite(limit) && limit > 0;
            const previousWidth = root.effectivePageWidth;
            const currentWidth = bounded ? Math.min(previousWidth, limit) : previousWidth;
            const newWidth = bounded ? Math.min(currentWidth + delta, limit) : currentWidth + delta;
            root.pageWidthDistance = Math.max(100, Math.floor(newWidth));
            root.pageWidthDistanceSaved(root.pageWidthDistance);
        };
        if (view.bookReady) {
            view.runJavaScript("globalThis.reader?.view?.renderer?.maxInlineSizeForViewport", applyWidth);
        } else {
            applyWidth(undefined);
        }
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

    function editorCommand() {
        return (Config.editorCommand || "").trim();
    }

    function canEditBook() {
        return !root.readOnly && editorCommand().length > 0 && root.filename !== "";
    }

    function bumpBookDeliveryVersion(version) {
        bookDeliveryReloadSerial += 1;
        bookDeliveryVersion = version && version.length > 0 ? version : Date.now() + "-" + bookDeliveryReloadSerial;
    }

    function editBook(selectedText, cfi, anchorId, anchorOpenTag, anchorCloseTag) {
        const editorAppFilePath = editorCommand();
        if (root.readOnly || editorAppFilePath.length === 0 || !root.filename) {
            return false;
        }

        const session = root.noLibraryImport ? { workingCopyPath: root.filename }
            : EditorSessionStore.createOrReuseEditorSession(bookServerIdentifier(), root.filename);
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
        if (root.noLibraryImport) {
            return ExternalProcess.startExternalEditor(editorAppFilePath, editorArguments);
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
        const session = EditorSessionStore.sessionInfo(sessionId);
        if (!session || session.error || (session.bookId !== root.bookServerIdentifier() && session.authoritativeFilePath !== root.filename)) {
            return;
        }
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
            importRepairDialog.showForImport(session.bookId, workingCopyPath, sessionId, true);
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
            if (importRepairDialog.visible) {
                importRepairDialog.actionStatus = message || "";
                importRepairDialog.showForImport(session.bookId, workingCopyPath, sessionId);
            } else {
                invalidAnchorImportDialog.open();
            }
            break;
        default:
            console.warn("Editor import returned unknown status:", status, message, "Recovery copy:", workingCopyPath);
            break;
        }
    }

    function openImportCorrections() {
        if (pendingInvalidImportSessionId) {
            const session = EditorSessionStore.sessionInfo(pendingInvalidImportSessionId);
            if (!session || session.error) return;
            importRepairDialog.showForImport(session.bookId, session.workingCopyPath, pendingInvalidImportSessionId);
        } else if (pendingActiveFileImportBookId) {
            importRepairDialog.showForImport(pendingActiveFileImportBookId, root.filename, "");
        }
    }

    function requestActiveFileImport(bookId, allowInvalidAnchors = true) {
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
                importRepairDialog.actionStatus = i18n("EPUB import completed.");
                importRepairDialog.showForImport(bookId, root.filename, "", true);
                return;
            }

            if (request.status === 409 && response) {
                root.pendingInvalidImportMessage = response.message || "";
                root.pendingInvalidImportIssues = response.anchorValidationIssues || [];
                if (importRepairDialog.visible) {
                    importRepairDialog.actionStatus = response.message || "";
                    importRepairDialog.recheck();
                } else {
                    invalidAnchorImportDialog.open();
                }
                return;
            }

            console.warn("Unable to import active EPUB state:", request.status, request.responseText);
            if (importRepairDialog.visible) {
                importRepairDialog.actionStatus = i18n("The EPUB could not be imported. Correct the reported anchors and try again.");
            }
        };
        request.send(JSON.stringify({ allowInvalidAnchors: allowInvalidAnchors }));
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
            + "&mathJaxSupport=" + (Config.mathJaxSupport ? "true" : "false")
            + "&mathJaxRendering=" + encodeURIComponent(JSON.stringify(root.mathJaxRenderingOptions))
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
        const modifiedBookIds = payload && payload.modifiedBookIds ? Array.from(payload.modifiedBookIds) : [];
        if (!currentBookId || modifiedBookIds.indexOf(currentBookId) === -1) {
            return;
        }

        // Finish the save callback/dialog first and coalesce file-change notices.
        reloadChangedBookTimer.restart();
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
        if (entry.pageWidthDistance !== undefined && entry.pageWidthDistance !== null) {
            root.pageWidthDistance = entry.pageWidthDistance;
        }
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
            pageMode: entry.pageMode || root.currentPageMode(),
            pageWidthDistance: root.pageWidthDistance
        };
    }

    function importReadOnlyBook(callback) {
        if (root.libraryManaged) {
            if (typeof callback === "function") {
                callback(root.entry);
            }
            return;
        }

        root.addToLibraryRequested(callback);
    }

    function refreshLibraryEntryFromFile() {
        if (!root.libraryManaged) return;
        const refreshedEntry = applicationWindow().bookListModel.refreshBookFromFile(root.filename);
        if (root.libraryManaged && refreshedEntry.filename && refreshedEntry.filename.length > 0) {
            root.entry = refreshedEntry;
        }
    }

    title: backend.metadata ? backend.metadata.title + (root.referenceSourceTitle ? ' ← ' + root.referenceSourceTitle : '') + (root.noLibraryImport ? ' · ' + i18n("Outside Library") : '') : ''
    padding: 0

    function executeEpubSearch() {
        root.initialSearch = null;
        if (root.searchText.length === 0) {
            view.runJavaScript(`find.clearHighlight()`);
            searchResultModel.clear();
            searchResultModel.loading = false;
            searchDialog.close();
            if (root.headerSearchField) {
                root.headerSearchField.forceActiveFocus();
            }
            return;
        }
        searchResultModel.search(root.searchText);
        searchDialog.open();
    }

    onUrlChanged: {
        speechControls.close();
        speech.stop();
        root.cancelContinuousSpeech();
        root.speechSelection = [];
        reloadBook();
    }
    onActiveLayerPageChanged: if (activeLayerPage) {
        resumeSuspendedReader();
    } else {
        speech.stop();
        root.cancelContinuousSpeech();
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

        function onMathJaxSupportChanged() {
            root.reloadCurrentBook();
        }
    }

    Connections {
        target: applicationWindow().bookListModel

        function onEntryDataUpdated(entry) {
            root.applyUpdatedBookEntry(entry);
        }
    }

    QQC2.Dialog {
        id: searchDialog
        parent: QQC2.Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(Kirigami.Units.gridUnit * 35, parent ? parent.width - Kirigami.Units.gridUnit * 2 : Kirigami.Units.gridUnit * 35)
        height: Math.min(Kirigami.Units.gridUnit * 20, parent ? parent.height - Kirigami.Units.gridUnit * 2 : Kirigami.Units.gridUnit * 20)
        modal: true
        title: i18nc("@title:dialog, %1 is the number of matches", "Search Results(%1)", searchResultModel.count)
        standardButtons: QQC2.Dialog.Close

        contentItem: ListView {
            model: searchResultModel
            clip: true
            QQC2.ScrollBar.vertical: QQC2.ScrollBar {}

            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.gridUnit * 2
                visible: searchResultModel.count === 0
                text: searchResultModel.loading ? i18n("Loading") : i18n("No search results")
                icon.name: "system-search-symbolic"
            }

            delegate: Delegates.RoundedItemDelegate {
                id: searchDelegate

                required property int index
                required property string sectionMarkup
                required property string markup
                required property string cfi

                width: ListView.view.width
                onClicked: {
                    searchResultModel.selectResult(index);
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
        }
    }

    Shortcut {
        sequence: "Ctrl+F"
        onActivated: {
            if (root.headerSearchField) {
                root.headerSearchField.forceActiveFocus();
                root.headerSearchField.selectAll();
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
                        property string annotationQuestions: model.questions || ""

                        onClicked: {
                            backend.goToAnnotation({ annotationId: annotationKey, value: annotationCfi, invalid: model.invalid });
                            const note = backend.annotationForValue(annotationKey);
                            if (note && note.noteHtml) richNoteDialog.openSaved(note, root.entry.uniqueIdentifier, false);
                            notesDialog.close();
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

                                Kirigami.Icon {
                                    objectName: "aiRemarkOverviewIcon"
                                    visible: model.ai
                                    source: "tools-wizard"
                                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                                    Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                                    Accessible.name: i18n("AI Remark")
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
                                    objectName: "aiRemarkOverviewEditButton"
                                    Layout.preferredWidth: Kirigami.Units.gridUnit * 2
                                    Layout.preferredHeight: Kirigami.Units.gridUnit * 2
                                    visible: model.ai
                                    display: QQC2.AbstractButton.IconOnly
                                    icon.name: "document-edit"
                                    text: i18n("Edit")
                                    enabled: root.libraryManaged && annotationKey.length > 0
                                    QQC2.ToolTip.text: text
                                    QQC2.ToolTip.visible: hovered
                                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                                    onClicked: {
                                        const note = backend.annotationForValue(annotationKey);
                                        if (note) richNoteDialog.openSaved(note, root.entry.uniqueIdentifier, true);
                                        notesDialog.close();
                                    }
                                }

                                QQC2.ToolButton {
                                    Layout.preferredWidth: Kirigami.Units.gridUnit * 2
                                    Layout.preferredHeight: Kirigami.Units.gridUnit * 2
                                    display: QQC2.AbstractButton.IconOnly
                                    icon.name: "edit-delete"
                                    text: i18n("Delete Annotation")
                                    enabled: root.libraryManaged && annotationKey.length > 0
                                    QQC2.ToolTip.text: text
                                    QQC2.ToolTip.visible: hovered
                                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                                    onClicked: backend.deleteAnnotation(annotationKey)
                                }
                            }

                            QQC2.Label {
                                objectName: "aiRemarkOverviewQuestions"
                                visible: annotationQuestions.length > 0
                                Layout.fillWidth: true
                                text: i18n("User questions (inferences):") + "\n" + annotationQuestions
                                textFormat: Text.PlainText
                                wrapMode: Text.WordWrap
                            }

                            QQC2.Label {
                                visible: annotationNote.length > 0 && !model.discussion
                                Layout.fillWidth: true
                                textFormat: Text.PlainText
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
    property list<Kirigami.Action> speechActions: [
        Kirigami.Action {
            objectName: "epubSpeechStartAction"
            text: speech.state === SpeechController.Paused ? i18nc("@action:intoolbar", "Resume Reading") : i18nc("@action:intoolbar", "Read Selection Aloud")
            icon.name: "media-playback-start"
            displayHint: Kirigami.DisplayHint.IconOnly | Kirigami.DisplayHint.KeepVisible
            enabled: speech.state === SpeechController.Paused || (root.speechSelection.length > 0)
            onTriggered: {
                if (speech.state === SpeechController.Paused) {
                    speech.resume();
                } else {
                    root.readSpeechSelection(root.speechSelection);
                }
            }
        },
        Kirigami.Action {
            objectName: "epubSpeechStopAction"
            text: i18nc("@action:intoolbar", "Stop Reading")
            icon.name: "media-playback-stop"
            displayHint: Kirigami.DisplayHint.IconOnly | Kirigami.DisplayHint.KeepVisible
            enabled: root.continuousSpeech || speech.state !== SpeechController.Idle
            onTriggered: {
                speech.stop();
                root.cancelContinuousSpeech();
            }
        },
        Kirigami.Action {
            objectName: "epubSpeechPauseAction"
            text: i18nc("@action:intoolbar", "Pause Reading")
            icon.name: "media-playback-pause"
            displayHint: Kirigami.DisplayHint.IconOnly | Kirigami.DisplayHint.KeepVisible
            enabled: speech.state === SpeechController.Speaking
            onTriggered: speech.pause()
        },
        Kirigami.Action {
            objectName: "epubSpeechModeAction"
            text: root.speechMode === 2 ? i18nc("@action:intoolbar", "Continuous Reading")
                : speech.repeatSelection ? i18nc("@action:intoolbar", "Repeat Selection") : i18nc("@action:intoolbar", "Read Selection Once")
            icon.name: root.speechMode === 2 ? "" : speech.repeatSelection ? "media-playlist-repeat" : "media-playlist-normal"
            icon.source: root.speechMode === 2 ? "qrc:/qt/qml/org/kde/arianna/qml/icons/speech-continuous.svg" : ""
            displayHint: Kirigami.DisplayHint.IconOnly | Kirigami.DisplayHint.KeepVisible
            checkable: true
            checked: root.speechMode === 1
            onTriggered: root.setSpeechMode((root.speechMode + 1) % 3)
        }
    ]

    property Kirigami.Action speechRateDecreaseAction: Kirigami.Action {
        objectName: "epubSpeechRateDecreaseAction"
        text: i18nc("@action:intoolbar", "Decrease speech rate (current: %1%)", (speech.speechRate > 0 ? "+" : "") + speech.speechRate)
        icon.name: "go-down"
        enabled: speech.speechRate > -100
        onTriggered: speech.adjustSpeechRate(-10)
    }

    property Kirigami.Action speechRateIncreaseAction: Kirigami.Action {
        objectName: "epubSpeechRateIncreaseAction"
        text: i18nc("@action:intoolbar", "Increase speech rate (current: %1%)", (speech.speechRate > 0 ? "+" : "") + speech.speechRate)
        icon.name: "go-up"
        enabled: speech.speechRate < 100
        onTriggered: speech.adjustSpeechRate(10)
    }

    SpeechControlWindow {
        id: speechControls
        objectName: "epubSpeechControlWindow"
        actionPrefix: "epub"
        controlActions: root.speechActions
        rateDecreaseAction: root.speechRateDecreaseAction
        rateIncreaseAction: root.speechRateIncreaseAction
        speechRate: speech.speechRate
        readerActive: root.activeLayerPage
        transientParent: applicationWindow()
        onStopRequested: {
            speech.stop();
            root.cancelContinuousSpeech();
        }
    }

    MathJaxMenu {
        id: viewerMathMenu
        objectName: "epubMathJaxContextControls"
        anchors.fill: parent
        baseUrl: mathWindow.runtimeBaseUrl
        profile: mathWindow.previewProfile
        menuContainer: imageContextMenu
        submenuContainer: mathContextActions
        onCopyRequested: text => Clipboard.savePlainText(text)
        onActionTriggered: (token, value, revision, request) => mathWindow.executeMenuAction(token, value, revision, request)
    }

    FootnoteWindow {
        id: footnoteWindow
        readerProfile: view.profile
        readerZoom: view.zoomFactor
        readerActive: root.activeLayerPage && !root.readerSuspended && view.bookReady
        transientParent: applicationWindow()
        onDismissed: requestId => view.runJavaScript("globalThis.reader?.closeFootnote(" + JSON.stringify(requestId) + ")")
        onGoToRequested: requestId => view.runJavaScript("globalThis.reader?.goToFootnote(" + JSON.stringify(requestId) + ")")
        onLinkActivated: (requestId, index) => view.runJavaScript("globalThis.reader?.followFootnoteLink(" + JSON.stringify(requestId) + ", " + index + ")")
    }

    MathJaxWindow {
        id: mathWindow
        runtimeBaseUrl: bookServerBaseUrl + "/mathjax/" + encodeURIComponent(bookServerSessionToken) + "/"
        renderingOptions: root.mathJaxRenderingOptions
        readerActive: root.activeLayerPage && !root.readerSuspended
        transientParent: applicationWindow()
        preferredSpeechLocale: speech.voices.find(voice => voice.id === Config.speechVoice)?.language || Qt.locale().name
        onReadAloudRequested: segment => root.readSpeechSelection([segment])
    }

    SpeechController {
        id: speech
        objectName: "epubSpeechController"
        onReadingExtended: (sessionId, requestId, lastSegment) => {
            if (!root.continuousSpeech || requestId !== String(root.speechStreamId) || sessionId !== speech.sessionId) return;
            view.runJavaScript("globalThis.reader?.startSpeechStream(" + JSON.stringify(root.speechStreamId)
                + ", " + JSON.stringify(lastSegment.cfi) + ", true, " + JSON.stringify(sessionId) + ")");
        }
        onSessionChanged: {
            root.finiteSpeechSentence = false;
            view.runJavaScript("globalThis.reader?.setSpeechSession(" + JSON.stringify(speech.sessionId) + ")");
        }
        onSegmentStarted: (sessionId, segment) => view.runJavaScript("globalThis.reader?.showSpeechSegment(" + JSON.stringify(sessionId) + ", " + JSON.stringify(segment) + ", " + speech.speechRate + ")")
        onIndexMarkReached: (sessionId, segmentId, mark) => view.runJavaScript("globalThis.reader?.showSpeechMark(" + JSON.stringify(sessionId) + ", " + JSON.stringify(segmentId) + ", " + JSON.stringify(mark) + ")")
        onMoreSegmentsRequested: (sessionId, preload) => {
            if (root.continuousSpeech) {
                view.runJavaScript("globalThis.reader?.nextSpeechBatch(" + JSON.stringify(root.speechStreamId) + ", " + JSON.stringify(sessionId) + ", " + JSON.stringify(preload) + ")");
            }
        }
        onSegmentFinished: (sessionId, segmentId) => {
            if (speech.state !== SpeechController.Paused) {
                view.runJavaScript("globalThis.reader?.clearSpeechHighlight(" + JSON.stringify(sessionId) + ", " + JSON.stringify(segmentId) + ")");
            }
        }
        onStateChanged: {
            view.runJavaScript("globalThis.reader?.setSpeechPaused(" + (speech.state === SpeechController.Paused) + ")");
            if (speech.state === SpeechController.Speaking && root.speechMode === 2
                && !root.continuousSpeech && root.finiteSpeechSentence) {
                // Repeat can start asynchronously after the retained sentence
                // ended. Honour a further mode click made before its BEGIN.
                root.setSpeechMode(2);
            } else if (speech.state === SpeechController.Idle) {
                root.cancelContinuousSpeech();
                view.runJavaScript("globalThis.reader?.clearSpeechHighlight(" + JSON.stringify(speech.sessionId) + ")");
            }
        }
        onErrorOccurred: message => applicationWindow().showPassiveNotification(message, "long")
    }

    QQC2.ActionGroup {
        id: readerThemeActionGroup
        exclusive: true
    }

    property bool hasWorkStructure: false
    property bool coordinateBusy: false
    QQC2.Dialog {
        id: workCoordinateDialog
        parent: QQC2.Overlay.overlay
        title: i18n("Go to Work Coordinate")
        modal: true
        standardButtons: QQC2.Dialog.Close
        contentItem: ColumnLayout {
            QQC2.Label { text: i18n("Coordinate, e.g. १-४"); textFormat: Text.PlainText }
            QQC2.TextField {
                id: workCoordinateInput
                objectName: "workCoordinateInput"
                enabled: !root.coordinateBusy
                onAccepted: workCoordinateButton.clicked()
            }
            QQC2.Label { id: workCoordinateStatus; textFormat: Text.PlainText }
            QQC2.Button {
                id: workCoordinateButton
                objectName: "workCoordinateGoButton"
                text: i18n("Go")
                enabled: !root.coordinateBusy && workCoordinateInput.text.trim().length > 0
                onClicked: {
                    root.coordinateBusy = true;
                    view.runJavaScript("globalThis.navigateWorkCoordinate?.(" + JSON.stringify(workCoordinateInput.text) + ")");
                }
            }
        }
    }
    Connections {
        target: applicationWindow()
        ignoreUnknownSignals: true
        function onWorkStructureSaved(bookId) {
            if (bookId === root.bookServerIdentifier()) root.reloadCurrentBook();
        }
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
            id: epubSearchAction
            text: i18nc("@action:intoolbar", "Search")
            icon.name: "system-search-symbolic"
            displayHint: Kirigami.DisplayHint.KeepVisible
            onTriggered: searchDialog.open()

            displayComponent: RowLayout {
                spacing: Kirigami.Units.smallSpacing

                QQC2.ToolButton {
                    action: epubSearchAction
                    display: QQC2.AbstractButton.IconOnly
                    QQC2.ToolTip.text: epubSearchAction.text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }

                QQC2.TextField {
                    id: headerEpubSearchField
                    objectName: "epubHeaderSearchField"
                    Layout.minimumWidth: Kirigami.Units.gridUnit * 8
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                    selectByMouse: true
                    placeholderText: i18nc("@action:intoolbar", "Search")
                    text: root.searchText
                    onTextChanged: if (root.searchText !== text) root.searchText = text
                    onAccepted: root.executeEpubSearch()
                    Component.onCompleted: root.headerSearchField = headerEpubSearchField
                    Component.onDestruction: if (root.headerSearchField === headerEpubSearchField) root.headerSearchField = null
                }
            }
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
            enabled: root.canEditBook()
            onTriggered: root.editBook()
        },
        Kirigami.Action {
            text: i18nc("@action:intoolbar", "Add to Library")
            displayHint: Kirigami.DisplayHint.IconOnly
            icon.name: "list-add"
            visible: root.readOnly || root.noLibraryImport
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
                    readOnly: !root.libraryManaged,
                    importToLibrary: function(done) {
                        root.importReadOnlyBook(done);
                    }
                });
            }
        },
        Kirigami.Action {
            objectName: "workCoordinateAction"
            text: i18n("Go to Work Coordinate")
            icon.name: "go-jump"
            displayHint: Kirigami.DisplayHint.IconOnly
            visible: root.hasWorkStructure
            onTriggered: { workCoordinateStatus.text = ""; workCoordinateDialog.open(); }
        }
    ]

    SearchModel {
        id: searchResultModel
        objectName: "epubSearchModel"

        onResultSelected: (cfi, flash) => view.goToSearchResult(cfi, flash)

        onSearchTriggered: text => {
            if (view.bookReady) {
                const targetCfi = root.initialSearch ? root.initialSearch.location : "";
                view.runJavaScript(`reader.search(${JSON.stringify(text)}, {targetCfi: ${JSON.stringify(targetCfi)}})`);
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
        function onTranslationReady(requestId, translatedString, hasTranscription) {
            if (requestId !== root.translationRequestId) {
                return;
            }

            root.translationRequestId = 0;
            backend.showTranslation(translatedString, hasTranscription);
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
        id: mathSettingsReload
        interval: 400
        onTriggered: root.reloadCurrentBook()
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
        objectName: "translatorDialog"
        property bool hasTranscription: false
        visible: false
        title: hasTranscription ? i18nc("@title:window", "Transcription and Translation") : i18nc("@title:window", "Translation")
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
                    objectName: "translationResult"
                    textFormat: TextEdit.RichText
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
        objectName: "aiDiscussionDialog"
        property var contextSnapshot: ({})
        property var submissions: []
        property var savedDiscussion: null
        property string saveError: ""
        property var annotationTarget: ({})
        readonly property var latestSubmission: submissions.length ? submissions[submissions.length - 1] : null

        function updateSubmission(changes) {
            if (submissions.length === 0) return;
            const entries = submissions.slice();
            entries[entries.length - 1] = Object.assign({}, entries[entries.length - 1], changes);
            submissions = entries;
        }
        function recordSubmissionFailure() {
            if (submissions.length && submissions[submissions.length - 1].status === "submitted"
                && (CodexClient.state === CodexClient.Failed || CodexClient.state === CodexClient.Disconnected
                    || (CodexClient.state === CodexClient.Ready && CodexClient.errorString.length)))
                updateSubmission({status: "failed", error: CodexClient.errorString, answer: CodexClient.output});
        }
        parent: QQC2.ApplicationWindow.overlay
        modal: false
        closePolicy: QQC2.Popup.NoAutoClose
        title: i18n("AI Discussion")
        standardButtons: QQC2.Dialog.Close
        width: Math.min(Kirigami.Units.gridUnit * 44, parent ? parent.width - Kirigami.Units.gridUnit * 2 : Kirigami.Units.gridUnit * 44)
        height: Math.min(Kirigami.Units.gridUnit * 42, parent ? parent.height - Kirigami.Units.gridUnit * 2 : Kirigami.Units.gridUnit * 42)
        x: parent ? Math.round((parent.width - width) / 2) : 0
        y: parent ? Math.round((parent.height - height) / 2) : 0
        onOpened: aiDiscussionSession.connectToCodex()
        onClosed: {
            if (submissions.length && submissions[submissions.length - 1].status === "submitted")
                updateSubmission({status: "closed", answer: CodexClient.output});
            CodexClient.disconnectFromServer();
        }
        Component.onDestruction: {
            if (visible) CodexClient.disconnectFromServer();
        }

        Connections {
            target: CodexClient
            enabled: aiDiscussionDialog.visible
            function onRequestCompleted(method, result) {
                if (method === "turn/start" && result.turn) aiDiscussionDialog.updateSubmission({turnId: result.turn.id});
            }
            function onTurnFinished(threadId, turnId, status) {
                aiDiscussionDialog.updateSubmission({threadId: threadId, turnId: turnId, status: status, answer: CodexClient.output});
            }
            function onStateChanged() {
                aiDiscussionDialog.recordSubmissionFailure();
            }
            function onErrorStringChanged() {
                aiDiscussionDialog.recordSubmissionFailure();
            }
        }

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            QQC2.Label {
                id: aiDiscussionSelectedTextLabel
                Layout.fillWidth: true
                text: i18n("Selected text:")
                wrapMode: Text.WordWrap
                font.weight: Font.Medium
            }

            QQC2.ScrollView {
                id: aiDiscussionSelectedTextScroll
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 5
                clip: true

                QQC2.TextArea {
                    id: aiDiscussionSelectedText
                    objectName: "aiDiscussionSelectedText"
                    textFormat: TextEdit.PlainText
                    width: aiDiscussionSelectedTextScroll.availableWidth
                    selectByMouse: true
                    wrapMode: Text.Wrap
                    placeholderText: i18n("No selected text available")
                }
            }

            ReaderContextPane {
                id: readerContextPane
                objectName: "readerContextPane"
                Layout.fillWidth: true
                snapshot: aiDiscussionDialog.contextSnapshot
                editedText: aiDiscussionSelectedText.text
                submissions: aiDiscussionDialog.submissions
                canRefresh: backend.canRefreshReaderContext()
                onRefreshRequested: backend.refreshReaderContext()
            }

            QQC2.Label {
                Layout.fillWidth: true
                text: i18n("Question:")
                font.weight: Font.Medium
            }

            QQC2.ScrollView {
                id: aiDiscussionQuestionPromptScroll
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 5
                clip: true

                QQC2.TextArea {
                    id: aiDiscussionQuestionPrompt
                    objectName: "aiDiscussionQuestionPrompt"
                    textFormat: TextEdit.PlainText
                    width: aiDiscussionQuestionPromptScroll.availableWidth
                    selectByMouse: true
                    wrapMode: Text.Wrap
                    placeholderText: i18n("Enter a question or task for the AI discussion")
                }
            }

            CodexSessionPane {
                id: aiDiscussionSession
                Layout.fillWidth: true
                Layout.fillHeight: true
                prompt: aiDiscussionQuestionPrompt.text
                readerContext: readerContextPane.context
                onContextSubmitted: (context, question, threadId) => {
                    aiDiscussionDialog.submissions = aiDiscussionDialog.submissions.concat([{
                        context: JSON.parse(ReaderContextBuilder.toJson(context)), question: question,
                        annotationTarget: JSON.parse(JSON.stringify(aiDiscussionDialog.annotationTarget)),
                        threadId: threadId, turnId: "", status: "submitted", answer: "", submittedAt: new Date().toISOString()
                    }]);
                }
            }

            QQC2.Button {
                objectName: "aiDiscussionSaveConversationButton"
                Layout.fillWidth: true
                text: i18n("Save Discussion as Remark")
                icon.name: "document-save"
                enabled: root.libraryManaged && !CodexClient.busy && !root.savingRichRemark
                    && aiDiscussionDialog.latestSubmission?.status !== "submitted"
                    && aiDiscussionDialog.submissions.some(turn => !!turn.answer?.trim())
                    && aiDiscussionDialog.latestSubmission?.annotationTarget?.bookId === root.entry.uniqueIdentifier
                    && !!aiDiscussionDialog.latestSubmission?.annotationTarget?.expectedStateId
                onClicked: backend.saveDiscussionRemark()
            }

            QQC2.Label {
                Layout.fillWidth: true
                visible: aiDiscussionDialog.saveError.length > 0
                text: aiDiscussionDialog.saveError
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Kirigami.Theme.negativeTextColor
            }

            RowLayout {
                Layout.fillWidth: true

                QQC2.Button {
                    objectName: "aiDiscussionSaveRemarkButton"
                    text: i18n("Save Answer as Remark…")
                    enabled: root.libraryManaged && !CodexClient.busy
                        && aiDiscussionDialog.latestSubmission?.status === "completed"
                        && !!aiDiscussionDialog.latestSubmission?.answer?.trim()
                        && aiDiscussionDialog.latestSubmission?.annotationTarget?.bookId === root.entry.uniqueIdentifier
                        && !!aiDiscussionDialog.latestSubmission?.annotationTarget?.expectedStateId
                    onClicked: {
                        const submission = aiDiscussionDialog.latestSubmission;
                        const saved = submission.annotationId ? backend.annotationForValue(submission.annotationId) : null;
                        if (saved) richNoteDialog.openSaved(saved, root.entry.uniqueIdentifier, true);
                        else richNoteDialog.openDraft(submission.answer, backend.reusableAiAnnotationTarget(submission.annotationTarget), {
                            schemaVersion: 1, origin: "ai-discussion", question: submission.question,
                            originalAnswer: submission.answer, threadId: submission.threadId, turnId: submission.turnId,
                            context: submission.context
                        });
                    }
                }

                Item {
                    Layout.fillWidth: true
                }

                QQC2.Button {
                    objectName: "aiDiscussionCopyPromptButton"
                    text: i18n("Copy Prompt")
                    icon.name: "edit-copy"
                    enabled: aiDiscussionSelectedText.text.trim().length > 0
                        && aiDiscussionQuestionPrompt.text.trim().length > 0
                    onClicked: backend.copyAiDiscussionPrompt()
                }
            }
        }
    }

    CrossReferenceEditor {
        id: crossReferenceEditor
        serverUrl: String(bookServerBaseUrl)
        sessionToken: String(bookServerSessionToken)
        onSaved: (response, edited) => {
            backend.loadReferenceOverview();
            root.reloadCurrentBookIfModified({reason: edited ? "reference-anchor-updated" : "reference-anchor-created",
                modifiedBookIds: response.modifiedBookIds, response: response});
        }
        onManualAnchorRequired: response => backend.dispatch({type: "manual-anchor-required", payload: response})
    }

    CrossReferenceDialog {
        id: crossReferenceDialog
        serverUrl: String(bookServerBaseUrl)
        sessionToken: String(bookServerSessionToken)
        canEdit: root.libraryManaged
        onOpenRequested: context => backend.openReferencePage(context.targetNavigationLocation, context.targetEntry,
            context.reference.target?.readOnly ?? false, context.sourceTitle)
        onEditRequested: context => backend.openCrossReferenceEditor(sourceBookId, "", context.text, {
            editMode: true, sourceAnchorId: context.ref, sourceAnchorTitle: context.sourceAnchorTitle,
            targetBookId: context.targetBookId, targetLocation: context.targetLocation,
            targetHrefLocation: context.targetHrefLocation, targetCfiLocation: context.targetCfiLocation,
            targetPreviewHtml: context.targetPreviewHtml, targetAnchorRepair: context.targetAnchorRepair
        })
        onRemoved: response => {
            backend.loadReferenceOverview();
            root.reloadCurrentBookIfModified({reason: "reference-anchor-deleted", modifiedBookIds: response.modifiedBookIds, response: response});
        }
    }

    RichNoteDialog {
        id: richNoteDialog
        parent: QQC2.ApplicationWindow.overlay
        canEdit: root.libraryManaged && bookId === root.entry.uniqueIdentifier
        onSaveRequested: (bookId, annotation, markdown) => saveRemark(bookId, annotation, markdown, false)
        onSaveDiscussionRequested: (bookId, annotation, turns) => saveRemark(bookId, annotation, turns, true)
        function saveRemark(bookId, annotation, content, conversation) {
            if (!canEdit) return;
            root.savingRichRemark = true;
            const result = conversation ? AnnotationStore.saveAiDiscussionAnnotation(bookId, annotation, content)
                : AnnotationStore.saveAiAnnotation(bookId, annotation, content);
            // Store signals are synchronous. Do not resolve a newly committed
            // anchor against the reader document from before that commit.
            if (result?.anchorId && !annotation.anchorId) root.reloadCurrentBook();
            root.savingRichRemark = false;
            if (!result || result.error) {
                if (result) richNoteDialog.annotation = Object.assign({}, annotation, result);
                richNoteDialog.errorMessage = result?.message || i18n("The remark could not be saved.");
                return;
            }
            backend.rememberAnnotation(result);
            const latest = aiDiscussionDialog.latestSubmission;
            if (!conversation && latest && latest.annotationTarget?.cfiRange === result.cfiRange) {
                aiDiscussionDialog.updateSubmission({annotationId: result.annotationId});
                aiDiscussionDialog.annotationTarget = Object.assign({}, aiDiscussionDialog.annotationTarget, {
                    anchorId: result.anchorId, expectedStateId: AnnotationStore.currentStateId(bookId)
                });
            }
            richNoteDialog.openSaved(result, bookId, false);
        }
    }

    Kirigami.Dialog {
        id: annotationDialog
        objectName: "epubAnnotationDialog"
        parent: QQC2.ApplicationWindow.overlay
        // parent: root
        modal: true
        title: i18nc("@title:dialog", "Annotation")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        width: Math.min(Kirigami.Units.gridUnit * 30, parent ? parent.width - Kirigami.Units.gridUnit * 2 : Kirigami.Units.gridUnit * 30)
        x: parent ? Math.round((parent.width - width) / 2) : 0
        y: parent ? Math.round((parent.height - height) / 2) : 0

        property string annotationKey: ""
        property var newSelection: null
        property string expectedStateId: ""

        onAccepted: {
            if (newSelection) {
                const source = newSelection;
                const state = expectedStateId;
                if (!backend.createAnnotationFromSelection(annotationColors.selectedColor, source, state, annotationNote.text)) {
                    // Keep the draft if the book changed while the dialog was open.
                    Qt.callLater(() => { newSelection = source; expectedStateId = state; open(); });
                }
            } else backend.updateAnnotationNote(annotationKey, annotationNote.text, annotationColors.selectedColor);
        }
        onOpened: annotationNote.forceActiveFocus()
        onClosed: newSelection = null

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
                objectName: "epubAnnotationNote"
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 8
                wrapMode: Text.Wrap
                placeholderText: i18n("Add Note…")
            }
            AnnotationColorPalette {
                id: annotationColors
                objectName: "epubAnnotationColors"
            }
        }
    }

    QQC2.Dialog {
        id: anchorEditNotice
        objectName: "anchorEditNotice"
        parent: QQC2.Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: i18n("Anchor Change Blocked")
        standardButtons: QQC2.Dialog.Close
        width: Math.min(Kirigami.Units.gridUnit * 40, parent ? parent.width - Kirigami.Units.gridUnit * 2 : 720)
        property string message: ""
        contentItem: QQC2.Label {
            text: anchorEditNotice.message
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
        }
    }

    EpubImportRepairDialog {
        id: importRepairDialog
        parent: root.QQC2.Overlay.overlay ? root.QQC2.Overlay.overlay : root
        onRetryActiveImportRequested: bookId => root.requestActiveFileImport(bookId, false)
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

            QQC2.Button {
                objectName: "correctImportCrossReferences"
                text: i18n("Check and Correct Cross-References")
                icon.name: "edit-link"
                onClicked: {
                    invalidAnchorImportDialog.close();
                    root.openImportCorrections();
                }
            }

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
        onReaderReadyChanged: if (readerReady) {
            if (Config.speechVoice) speech.refreshVoices();
            root.syncMathSpeechOptions();
        }
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
            readerTargetPopup.close();
            annotationPopup.close();
            referencePopup.close();
            readerLinkPopup.close();
            if (!crossReferenceEditor.saving) crossReferenceEditor.close();
            crossReferenceDialog.close();
            root.hasWorkStructure = false;
            root.coordinateBusy = false;
            workCoordinateDialog.close();
            mathWindow.reset();
            footnoteWindow.reset("");
            backend.readerContextSelection = null;
            speech.stop();
            root.speechSelection = [];
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
            ExternalProcess.clearWatchedFile(true);
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
            mathWindow.cancelContextMenu();
            imageContextMenu.formulaContext = false;
            imageContextMenu.nativeImageActions = true;
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

        function goToSearchResult(cfi, flash) {
            if (bookReady) {
                view.runJavaScript('reader.view.goToSearchResult(' + JSON.stringify(cfi) + ', ' + JSON.stringify(flash) + ').catch(error => console.error("reader.view.goToSearchResult failed", error?.stack ?? error))');
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
            root.zoomLevel = view.zoomFactor;
            root.zoomLevelSaved(view.zoomFactor);
        }

        function zoomIn() {
            setZoomFactor(view.zoomFactor + 0.1);
        }

        function zoomOut() {
            setZoomFactor(view.zoomFactor - 0.1);
        }

        function resolveSelectionMenu(menu, action, afterClose) {
            menu.actionTriggered = true;
            const requestId = menu.requestId;
            if (backend.selection?.requestId === menu.requestId) backend.selection = null;
            // Opening the next popup before this one finishes closing lets the
            // old popup restore focus over the new one and swallow Escape.
            const resolve = () => {
                menu.closed.disconnect(resolve);
                view.runJavaScript("globalThis.selectionAction?.(" + JSON.stringify(action) + "," + requestId + ")");
                if (afterClose) afterClose();
            };
            menu.closed.connect(resolve);
            menu.close();
        }

        function cancelSelectionMenu(menu) {
            if (!menu.actionTriggered) {
                if (backend.selection?.requestId === menu.requestId) backend.selection = null;
                view.runJavaScript("globalThis.selectionAction?.(undefined," + menu.requestId + ")");
            }
            menu.actionTriggered = false;
        }

        Connections {
            target: backend
            function onSelectionChanged() {
                const requestId = backend.selection?.requestId;
                Qt.callLater(() => {
                    if (!backend.selection || backend.selection.requestId !== requestId) {
                        return;
                    }

                    if (backend.selection.type === "reader-targets") {
                        readerTargetPopup.choices = backend.selection.choices || [];
                        readerTargetPopup.requestId = backend.selection.requestId;
                        readerTargetPopup.popup();
                    } else if (backend.selection.type === "annotation") {
                        const note = backend.annotationForValue(backend.selection.annotationKey || backend.selection.value);
                        if (note && note.noteHtml && !backend.selection.contextMenu) {
                            richNoteDialog.openSaved(note, root.entry.uniqueIdentifier, false);
                            backend.selection = null;
                            view.runJavaScript("globalThis.selectionAction?.()");
                        } else {
                            annotationPopup.selectionSnapshot = backend.selection;
                            annotationPopup.requestId = backend.selection.requestId;
                            annotationPopup.popup();
                        }
                    } else if (backend.selection.type === "reference") {
                        referencePopup.requestId = backend.selection.requestId;
                        referencePopup.popup();
                    } else if (backend.selection.type === "reader-link") {
                        readerLinkPopup.href = backend.selection.href || "";
                        readerLinkPopup.requestId = backend.selection.requestId;
                        readerLinkPopup.popup();
                    } else {
                        selectionPopup.popup();
                    }
                });
            }
        }

        QQC2.Menu {
            id: readerTargetPopup
            objectName: "readerTargetPopup"

            property var choices: []
            property int requestId: 0
            property bool actionTriggered: false

            function resolveTarget(index) {
                view.resolveSelectionMenu(readerTargetPopup, index);
            }

            onOpened: actionTriggered = false
            onClosed: view.cancelSelectionMenu(readerTargetPopup)

            Instantiator {
                model: readerTargetPopup.choices
                delegate: QQC2.MenuItem {
                    required property var modelData
                    required property int index
                    objectName: "readerTargetChoice" + index
                    text: modelData.kind === "annotation" ? i18n("Remark")
                        : modelData.kind === "reference" ? i18n("Cross-reference") : i18n("Formula")
                    icon.name: modelData.kind === "annotation" ? "comment"
                        : modelData.kind === "reference" ? "insert-link" : "formula"
                    QQC2.ToolTip.text: String(modelData.text || "").replace(/\s+/g, " ").slice(0, 200)
                    Accessible.description: QQC2.ToolTip.text
                    QQC2.ToolTip.visible: hovered && QQC2.ToolTip.text.length > 0
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    // Resolve before MenuItem's automatic close can cancel the
                    // pending choice (native popup windows may close at once).
                    onClicked: readerTargetPopup.resolveTarget(index)
                }
                onObjectAdded: (index, object) => readerTargetPopup.insertItem(index, object)
                onObjectRemoved: (index, object) => readerTargetPopup.removeItem(object)
            }
        }

        QQC2.Menu {
            id: selectionPopup

            property bool actionTriggered: false
            property var highlightSelection: null

            function resolveSelection(action) {
                selectionPopup.actionTriggered = true;
                backend.selection = null;
                view.runJavaScript(action === undefined ? "selectionAction()" : "selectionAction(" + JSON.stringify(action) + ")");
                selectionPopup.close();
            }

            onOpened: actionTriggered = false
            onClosed: {
                if (!actionTriggered) {
                    view.runJavaScript("selectionAction()");
                }
                actionTriggered = false;
                backend.selection = null;
                const pending = highlightSelection;
                highlightSelection = null;
                if (pending) backend.openNewAnnotationEditor(pending);
            }

            QQC2.MenuItem {
                objectName: "epubHighlightMenuItem"
                text: i18n("Highlight")
                icon.name: 'bookmark-new'
                enabled: backend.canAnnotateSelection()
                onClicked: {
                    if (!backend.canAnnotateSelection()) return;
                    selectionPopup.highlightSelection = backend.cloneAnnotation(backend.selection);
                    selectionPopup.resolveSelection();
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
                objectName: "aiDiscussionMenuItem"
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
                enabled: root.canEditBook() && !!backend.selection && !!backend.selection.text
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
            QQC2.MenuItem {
                objectName: "epubReadSelectionMenuItem"
                text: i18nc("@action:inmenu", "Read Aloud")
                icon.source: "qrc:/qt/qml/org/kde/arianna/qml/icons/read-aloud.svg"
                icon.color: Kirigami.Theme.textColor
                // SVG formulas can have a speech segment without selected text.
                enabled: !!backend.selection && root.speechSelection.length > 0
                QQC2.ToolTip.text: i18nc("@info:tooltip", "Ctrl+click: Read from the selection to the end of the book")
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                onClicked: selectionPopup.resolveSelection("read-aloud")
                Keys.onPressed: event => {
                    if ((event.modifiers & Qt.ControlModifier) && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space)) {
                        selectionPopup.resolveSelection("read-document");
                        event.accepted = true;
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    onPressed: mouse => mouse.accepted = (mouse.modifiers & Qt.ControlModifier) !== 0
                    onClicked: selectionPopup.resolveSelection("read-document")
                }
            }
        }

        QQC2.Menu {
            id: annotationPopup
            objectName: "annotationPopup"
            focus: true
            property var selectionSnapshot: null
            property int requestId: 0
            property bool actionTriggered: false
            readonly property string annotationKey: selectionSnapshot?.annotationKey || selectionSnapshot?.value || ""
            readonly property var note: annotationKey ? backend.annotationForValue(annotationKey) : null

            onOpened: actionTriggered = false
            onClosed: view.cancelSelectionMenu(annotationPopup)

            QQC2.MenuItem {
                objectName: "openRemarkContextAction"
                text: i18n("Open Remark")
                icon.name: 'comment'
                visible: !!annotationPopup.note?.noteHtml
                onClicked: view.resolveSelectionMenu(annotationPopup, "open-annotation")
            }

            QQC2.MenuItem {
                objectName: "editRemarkContextAction"
                text: i18n("Edit Annotation")
                icon.name: 'pencil'
                enabled: !!annotationPopup.note
                onClicked: {
                    const key = annotationPopup.annotationKey;
                    view.resolveSelectionMenu(annotationPopup, "edit-annotation", () => backend.openAnnotationEditor(key));
                }
            }

            QQC2.MenuItem {
                text: i18n("Select Text")
                icon.name: 'edit-select-text'
                onClicked: view.resolveSelectionMenu(annotationPopup, "select")
            }

            QQC2.MenuItem {
                text: i18n("Delete Annotation")
                icon.name: 'edit-delete'
                enabled: !!annotationPopup.note
                onClicked: {
                    const key = annotationPopup.annotationKey;
                    view.resolveSelectionMenu(annotationPopup, "delete-annotation");
                    backend.deleteAnnotation(key);
                }
            }
        }

        QQC2.Menu {
            id: referencePopup
            objectName: "referencePopup"
            focus: true

            property int requestId: 0
            property bool actionTriggered: false

            function resolveReference(action) {
                view.resolveSelectionMenu(referencePopup, action);
            }

            onOpened: actionTriggered = false
            onClosed: view.cancelSelectionMenu(referencePopup)

            QQC2.MenuItem {
                text: i18n("Open Reference")
                icon.name: 'document-open'
                enabled: !!(backend.selection && backend.selection.canOpen)
                onClicked: referencePopup.resolveReference("open-reference")
            }

            QQC2.MenuItem {
                objectName: "editReferenceContextAction"
                text: i18n("Edit Reference")
                icon.name: 'document-edit'
                enabled: !!(backend.selection && backend.selection.ref)
                onClicked: referencePopup.resolveReference("edit-reference")
            }

            QQC2.MenuItem {
                text: i18n("Delete Reference")
                icon.name: 'edit-delete'
                enabled: !!(backend.selection && backend.selection.ref)
                onClicked: referencePopup.resolveReference("delete-reference")
            }
        }

        QQC2.Menu {
            id: readerLinkPopup
            objectName: "readerLinkPopup"
            focus: true
            property int requestId: 0
            property bool actionTriggered: false
            property string href: ""
            onOpened: actionTriggered = false
            onClosed: view.cancelSelectionMenu(readerLinkPopup)

            QQC2.MenuItem {
                objectName: "openReaderLinkContextAction"
                text: i18n("Open Link")
                icon.name: 'document-open'
                onClicked: view.resolveSelectionMenu(readerLinkPopup, "open-link")
            }
            QQC2.MenuItem {
                text: i18n("Copy Link")
                icon.name: 'edit-copy'
                onClicked: {
                    Clipboard.savePlainText(readerLinkPopup.href);
                    view.resolveSelectionMenu(readerLinkPopup, "copy-link");
                }
            }
        }

        QQC2.Menu {
            id: imageContextMenu
            objectName: "epubImageContextMenu"

            property bool nativeImageActions: true
            property bool formulaContext: false
            onClosed: mathWindow.cancelContextMenuIfIdle()

            QQC2.MenuItem {
                text: i18n("Back")
                visible: !imageContextMenu.formulaContext
                enabled: view.readerCanGoBack
                onClicked: view.goBack()
            }
            QQC2.MenuItem {
                text: i18n("Forward")
                visible: !imageContextMenu.formulaContext
                enabled: view.readerCanGoForward
                onClicked: view.goForward()
            }
            QQC2.MenuItem {
                text: i18n("Reload")
                visible: !imageContextMenu.formulaContext
                onClicked: view.triggerWebAction(WebEngineView.Reload)
            }
            QQC2.MenuItem {
                text: i18n("Save page")
                visible: !imageContextMenu.formulaContext
                onClicked: view.triggerWebAction(WebEngineView.SavePage)
            }
            QQC2.MenuSeparator { visible: !imageContextMenu.formulaContext }
            QQC2.MenuItem {
                objectName: "epubImageNotInverseAction"
                text: i18n("Set Image not_Inverse")
                visible: !imageContextMenu.formulaContext
                enabled: root.libraryManaged && root.bookServerIdentifier().length > 0
                onClicked: view.runJavaScript("reader.setContextImageNotInverse()")
            }
            QQC2.MenuItem {
                objectName: "epubOpenFormulaDialogAction"
                text: i18nc("@action:inmenu", "Open Formula Dialog")
                visible: imageContextMenu.formulaContext
                onClicked: mathWindow.showPreparedFormula()
            }
            QQC2.MenuItem {
                objectName: "epubReadFormulaAction"
                text: i18nc("@action:button", "Read Formula Aloud")
                visible: imageContextMenu.formulaContext
                enabled: mathWindow.status === "ready" && !!mathWindow.speechText
                onClicked: mathWindow.readCurrentFormula()
            }
            QQC2.Menu {
                id: mathContextActions
                objectName: "epubMathJaxContextActions"
                title: i18nc("@title:menu", "MathJax (Formula Preview)")
            }
            function syncFormulaContext() {
                // Qt represents a submenu with a generated MenuItem.
                for (let i = 0; i < count; ++i) {
                    if (menuAt(i) === mathContextActions) itemAt(i).visible = formulaContext;
                }
            }
            Component.onCompleted: syncFormulaContext()
            onFormulaContextChanged: syncFormulaContext()
            QQC2.MenuSeparator { visible: imageContextMenu.nativeImageActions }
            QQC2.MenuItem {
                text: i18n("Save image")
                visible: imageContextMenu.nativeImageActions
                onClicked: view.triggerWebAction(WebEngineView.DownloadImageToDisk)
            }
            QQC2.MenuItem {
                text: i18n("Copy image")
                visible: imageContextMenu.nativeImageActions
                onClicked: view.triggerWebAction(WebEngineView.CopyImageToClipboard)
            }
            QQC2.MenuItem {
                text: i18n("Copy image address")
                visible: imageContextMenu.nativeImageActions
                onClicked: view.triggerWebAction(WebEngineView.CopyImageUrlToClipboard)
            }
        }
    }

    // Vertical: pages / Ctrl: zoom / Shift: search results.
    // Horizontal: sections / Ctrl: width / Shift: remarks.
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
            const dx = event.angleDelta.x || event.pixelDelta.x;
            const dy = event.angleDelta.y || event.pixelDelta.y;
            if (Math.abs(dx) < 1 && Math.abs(dy) < 1) {
                event.accepted = false;
                return;
            }
            event.accepted = true;
            const horizontal = Math.abs(dx) > Math.abs(dy);
            const delta = horizontal ? dx : dy;
            if (event.modifiers === Qt.ShiftModifier) {
                if (horizontal) {
                    backend.navigateAnnotation(delta > 0);
                } else if (delta > 0) {
                    searchResultModel.previousResult(true);
                } else {
                    searchResultModel.nextResult(true);
                }
            } else if (event.modifiers & Qt.ControlModifier) {
                if (!horizontal) {
                    if (delta > 0)
                        view.zoomIn();
                    else
                        view.zoomOut();
                } else {
                    root.changePageWidth(delta > 0 ? 25 : -25, "STRG+horizontales Mausrad");
                }
            } else if (!horizontal) {
                if (delta > 0)
                    view.prev();
                else
                    view.next();
            } else {
                if (delta > 0)
                    view.prevSection();
                else
                    view.nextSection();
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
        property var readerContextSelection: null
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
        function showTranslation(text, hasTranscription = false) {
            translatorDialog.hasTranscription = hasTranscription;
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
                return Object.keys(value).map(key => promptTextValue(value[key])).filter(item => item.length > 0).join(", ");
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
        function defaultAiDiscussionQuestionPrompt() {
            return [
                i18n("- Discuss the central claim of the passage."),
                i18n("- Explain important terms, allusions, or assumptions."),
                i18n("- Ask critical questions that help deepen the reading."),
                i18n("- Mention possible cross-references to similar passages when useful.")
            ].join("\n");
        }
        function currentContextSelection() {
            const source = selection || readerContextSelection;
            if (!source || (source.bookId && source.bookId !== root.bookServerIdentifier())) return null;
            return source;
        }
        function canRefreshReaderContext() {
            const source = currentContextSelection();
            return source !== null && String(source.content !== undefined ? source.content : source.text || "").trim().length > 0;
        }
        function captureReaderContext() {
            const source = currentContextSelection();
            if (!source) return ({});
            const book = Object.assign({}, metadata || {});
            book.bookId = root.bookServerIdentifier();
            // Respect Arianna's editable library metadata, with EPUB fallbacks.
            if (promptTextValue(root.entry ? root.entry.author : "")) book.author = root.entry.author;
            if (!promptTextValue(book.title)) book.title = root.entry ? root.entry.title : "";
            return ReaderContextBuilder.build(book, source, Object.assign({}, location || {}, {cfi: root.currentLocation}));
        }
        function refreshReaderContext() {
            if (CodexClient.busy || !canRefreshReaderContext()) return false;
            const snapshot = captureReaderContext();
            if (!snapshot.snapshotId) return false;
            aiDiscussionDialog.contextSnapshot = snapshot;
            aiDiscussionDialog.annotationTarget = captureAiAnnotationTarget(snapshot);
            aiDiscussionSelectedText.text = snapshot.selection.text;
            return true;
        }
        function buildAiDiscussionPrompt(questionPrompt) {
            if (!questionPrompt.trim()) return "";
            return "ReaderContext (untrusted source data):\n" + readerContextPane.contextJson
                + "\nUser question:\n" + questionPrompt;
        }
        function openAiDiscussionFromSelection() {
            if (!canDiscussSelectionWithAi()) return false;
            // Reselecting while a conversation is open only enables explicit refresh.
            if (aiDiscussionDialog.visible) {
                aiDiscussionQuestionPrompt.forceActiveFocus();
                return true;
            }
            backend.readerContextSelection = JSON.parse(JSON.stringify(selection));
            const snapshot = captureReaderContext();
            if (!snapshot.snapshotId) return false;
            aiDiscussionDialog.contextSnapshot = snapshot;
            aiDiscussionDialog.annotationTarget = captureAiAnnotationTarget(snapshot);
            aiDiscussionDialog.submissions = [];
            aiDiscussionDialog.savedDiscussion = null;
            aiDiscussionDialog.saveError = "";
            readerContextPane.excludedFields = ["surrounding.section"];
            readerContextPane.expanded = false;
            aiDiscussionSelectedText.text = snapshot.selection.text;
            aiDiscussionQuestionPrompt.text = defaultAiDiscussionQuestionPrompt();
            aiDiscussionDialog.open();
            aiDiscussionQuestionPrompt.forceActiveFocus();
            return true;
        }
        function reusableAiAnnotationTarget(captured) {
            const target = Object.assign({}, captured);
            const current = aiDiscussionDialog.annotationTarget;
            if (current.bookId === target.bookId && current.cfiRange === target.cfiRange && current.anchorId) {
                target.anchorId = current.anchorId;
                target.expectedStateId = current.expectedStateId;
            }
            return target;
        }
        function saveDiscussionRemark() {
            const latest = aiDiscussionDialog.latestSubmission;
            if (!root.libraryManaged || root.savingRichRemark || CodexClient.busy || !latest
                || latest.annotationTarget?.bookId !== root.entry.uniqueIdentifier) return;
            const bookId = latest.annotationTarget.bookId;
            // Bind to the selection captured when this question was sent.
            let target = reusableAiAnnotationTarget(latest.annotationTarget);
            const saved = aiDiscussionDialog.savedDiscussion;
            if (saved?.bookId === bookId && saved.cfiRange === target.cfiRange) {
                target = Object.assign({}, target, saved);
            }
            root.savingRichRemark = true;
            const result = AnnotationStore.saveAiDiscussionAnnotation(bookId, target, aiDiscussionDialog.submissions);
            if (result?.anchorId && !target.anchorId) root.reloadCurrentBook();
            root.savingRichRemark = false;
            if (!result || result.error) {
                // Preserve an anchor created before a database error for a safe retry.
                if (result?.anchorId) aiDiscussionDialog.savedDiscussion = Object.assign({}, result, {bookId: bookId});
                aiDiscussionDialog.saveError = result?.message || i18n("The remark could not be saved.");
                return;
            }
            aiDiscussionDialog.saveError = "";
            aiDiscussionDialog.savedDiscussion = Object.assign({}, result, {bookId: bookId});
            backend.rememberAnnotation(result);
            if (aiDiscussionDialog.annotationTarget.bookId === bookId
                && aiDiscussionDialog.annotationTarget.cfiRange === result.cfiRange)
                aiDiscussionDialog.annotationTarget = Object.assign({}, aiDiscussionDialog.annotationTarget, {
                    anchorId: result.anchorId, expectedStateId: AnnotationStore.currentStateId(bookId)
                });
            richNoteDialog.openSaved(result, bookId, false);
        }
        function copyAiDiscussionPrompt() {
            const prompt = buildAiDiscussionPrompt(aiDiscussionQuestionPrompt.text);
            if (prompt.length > 0) Clipboard.saveText(prompt);
        }
        function captureAiAnnotationTarget(snapshot) {
            const bookId = snapshot.bibliography?.bookId || "";
            return {
                bookId: bookId,
                cfiRange: snapshot.location?.selectionCfi || "",
                text: snapshot.selection?.text || "",
                expectedStateId: root.libraryManaged && bookId ? AnnotationStore.currentStateId(bookId) : "",
                color: "yellow"
            };
        }
        function openCrossReferenceEditor(bookId, cfi, text, options) {
            if (root.libraryManaged && bookId === root.bookServerIdentifier())
                crossReferenceEditor.begin(bookId, cfi, text, options);
        }
        function showCrossReference(bookId, context, deletion) {
            if (bookId === root.bookServerIdentifier())
                crossReferenceDialog.showReference(bookId, context, deletion);
        }
        function openReferencePage(location, entry, readOnly, sourceTitle) {
            if (!location) {
                console.warn('Cannot open reference page, missing location');
                return;
            }

            const referenceEntry = entry && entry.filename ? entry : root.entry;
            const referenceFilename = referenceEntry && referenceEntry.filename ? referenceEntry.filename : root.filename;
            const referenceLocations = entry && entry.locations ? entry.locations : root.locations;
            const externalReference = root.noLibraryImport && referenceFilename === root.filename;
            const referenceReadOnly = root.readOnly || (readOnly === true && !externalReference);

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
                pageWidthDistance: referenceEntry && referenceEntry.pageWidthDistance >= 0 ? referenceEntry.pageWidthDistance : -1,
                filename: referenceFilename,
                entry: referenceEntry,
                readOnly: referenceReadOnly,
                noLibraryImport: externalReference,
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
        function goToAnnotation(annotation) {
            annotation = annotationForValue(annotation.annotationId || annotationKey(annotation)) || annotation;
            if (view.bookReady) {
                view.runJavaScript("reader.goToAnnotation(" + JSON.stringify(annotation) + ").catch(error => console.warn('Unable to navigate to remark', error))");
            }
        }
        function navigateAnnotation(previous) {
            if (view.bookReady) {
                view.runJavaScript("reader.navigateAnnotation(" + JSON.stringify(annotationList()) + ", " + previous + ")");
            }
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
                    ai: annotation.noteMetadata?.origin === "ai-discussion",
                    discussion: annotation.noteMetadata?.kind === "conversation",
                    questions: Array.from(AnnotationStore.discussionQuestions(annotation)).map(question => "• " + question).join("\n"),
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
            if (root.savingRichRemark || !view.bookReady || !annotation || annotation.invalid || (!annotation.anchorId && !annotation.value && !annotation.cfiRange)) {
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
        function canAnnotateSelection(source = selection) {
            return root.libraryManaged && root.entry.uniqueIdentifier.length > 0 && source && source.type === "selection" && source.value
                && (!source.bookId || source.bookId === root.entry.uniqueIdentifier);
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
            if (!annotation || (!annotation.anchorId && !annotation.value && !annotation.cfiRange) || !root.libraryManaged || root.entry.uniqueIdentifier.length === 0) {
                return false;
            }

            const copy = cloneAnnotation(annotation);
            AnnotationStore.saveAnnotation(root.entry.uniqueIdentifier, copy);
            return rememberAnnotation(copy);
        }
        function openNewAnnotationEditor(source) {
            if (!canAnnotateSelection(source)) return;
            const existing = annotationForValue(source.value);
            if (existing) {
                openAnnotationEditor(annotationKey(existing));
                return;
            }
            annotationDialog.annotationKey = "";
            annotationDialog.newSelection = cloneAnnotation(source);
            annotationDialog.expectedStateId = AnnotationStore.currentStateId(root.entry.uniqueIdentifier);
            annotationExcerpt.text = source.content || source.text || "";
            annotationNote.text = "";
            annotationColors.selectedColor = "yellow";
            annotationDialog.open();
        }
        function createAnnotationFromSelection(color = "yellow", source = selection, expectedStateId = "", note = "") {
            if (!canAnnotateSelection(source)) {
                return null;
            }

            const existingAnnotation = annotationForValue(source.value);
            if (existingAnnotation) {
                return existingAnnotation;
            }

            const annotation = {
                value: source.value,
                cfiRange: source.value,
                color: color,
                text: source.content || source.text || "",
                note: note,
                created: new Date().toISOString(),
                modified: ""
            };
            if (!expectedStateId) expectedStateId = AnnotationStore.currentStateId(root.entry.uniqueIdentifier);
            root.savingRichRemark = true;
            const anchoredAnnotation = AnnotationStore.createAnchoredAnnotation(root.entry.uniqueIdentifier, annotation, expectedStateId);
            if (anchoredAnnotation?.anchorId && !anchoredAnnotation.error) root.reloadCurrentBook();
            root.savingRichRemark = false;
            if (!anchoredAnnotation || anchoredAnnotation.error) {
                console.warn("Unable to create anchored annotation:", anchoredAnnotation ? anchoredAnnotation.message || "" : "");
                if (anchoredAnnotation && anchoredAnnotation.message) {
                    anchorEditNotice.message = anchoredAnnotation.message;
                    anchorEditNotice.open();
                }
                return null;
            }

            return rememberAnnotation(anchoredAnnotation) ? anchoredAnnotation : null;
        }
        function updateAnnotationNote(key, note, color) {
            const annotation = annotationForValue(key);
            if (!annotation) {
                return;
            }

            const copy = cloneAnnotation(annotation);
            copy.note = note;
            if (color !== undefined) copy.color = color;
            copy.modified = new Date().toISOString();
            saveAnnotation(copy);
        }
        function deleteAnnotation(key) {
            const annotation = annotationForValue(key);
            if (!annotation || !root.libraryManaged || root.entry.uniqueIdentifier.length === 0) {
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
            if (annotation.noteHtml) {
                richNoteDialog.openSaved(annotation, root.entry.uniqueIdentifier, true);
                return;
            }

            annotationDialog.annotationKey = annotationKey(annotation);
            annotationDialog.newSelection = null;
            annotationExcerpt.text = annotation.text || "";
            annotationNote.text = annotation.note || "";
            annotationColors.selectedColor = annotation.color || "yellow";
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
            case 'show-footnote':
                footnoteWindow.open(action.payload);
                break;
            case 'style-footnote':
                footnoteWindow.applyAppearance(action.payload);
                break;
            case 'close-footnote':
                footnoteWindow.reset(action.payload?.requestId || "");
                break;
            case 'show-math':
                mathWindow.open(action.payload);
                break;
            case 'show-math-context-menu':
                if (root.activeLayerPage && !root.readerSuspended && Config.mathJaxSupport) {
                    imageContextMenu.formulaContext = true;
                    imageContextMenu.nativeImageActions = false;
                    mathWindow.openContextMenu(action.payload, viewerMathMenu);
                    imageContextMenu.popup();
                }
                break;
            case 'change-page-width':
                if (root.visible && !root.readerSuspended && (action.payload.delta === 25 || action.payload.delta === -25)) {
                    root.changePageWidth(action.payload.delta, action.payload.source);
                }
                break;
            case 'ready':
                const uiText = {
                    loc: i18n("Loc. %s of %s"),
                    page: i18n("Page %s of %s"),
                    pageWithoutTotal: i18n("Page %s"),
                    close: i18n("Close"),
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
                view.runJavaScript(`init({'uiText': ${JSON.stringify(uiText)}, 'resourceOrigin': ${JSON.stringify(bookServerBaseUrl)}})`);
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
                if (root.initialSearch && root.initialSearch.query) {
                    searchResultModel.search(root.initialSearch.query);
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
                } else if (action.payload.action === 'read-aloud') {
                    root.readSpeechSelection(action.payload.segments || []);
                } else if (action.payload.action === 'read-document') {
                    root.readDocumentFrom(action.payload.cfi);
                } else if (action.payload.action === 'translate') {
                    backend.showTranslation(i18n("Translating..."));
                    root.translationRequestId = action.payload.segments && action.payload.segments.length
                        ? Translator.translateSegments(action.payload.segments)
                        : Translator.translate(action.payload.text);
                } else {
                    backend.selection = action.payload;
                }
                break;
            case 'speech-batch': {
                const batch = action.payload;
                if (!root.continuousSpeech || batch.streamId !== root.speechStreamId) break;
                if (batch.error) {
                    speech.stop();
                    root.cancelContinuousSpeech();
                    applicationWindow().showPassiveNotification(batch.error, "long");
                } else if (batch.initial) {
                    if (!batch.segments.length && !batch.hasMore) root.cancelContinuousSpeech();
                    else speech.startReading(batch.segments, batch.hasMore);
                } else if (batch.sessionId === speech.sessionId) {
                    speech.continueReading(batch.sessionId, batch.segments, batch.hasMore);
                }
                break;
            }
            case 'speech-selection':
                root.speechSelection = action.payload.segments || [];
                break;
            case 'selection-find':
                root.searchText = action.payload.text;
                root.executeEpubSearch();
                break;
            case 'editor-working-copy-conflict':
                anchorEditNotice.message = action.payload || "";
                anchorEditNotice.open();
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
            case 'work-structure-ready':
                if (action.payload.bookId === root.bookServerIdentifier()) root.hasWorkStructure = action.payload.available;
                break;
            case 'work-coordinate-result':
                root.coordinateBusy = false;
                if (action.payload.resolved) workCoordinateDialog.close();
                else workCoordinateStatus.text = i18n("The coordinate has no unique confirmed target.");
                break;
            case 'book-reload-needed':
                root.reloadCurrentBookIfModified(action.payload);
                break;
            case 'image-not-inverse-error':
                applicationWindow().showPassiveNotification(i18n("Unable to save the image correction to the EPUB: %1", action.payload), "long");
                break;
            case 'create-overlay':
                backend.renderAnnotations();
                break;
            case 'annotation-unresolved': {
                if (action.payload.bookId !== root.bookServerIdentifier()) break;
                const note = backend.annotationForValue(action.payload.annotationId);
                if (note && !note.invalid) {
                    backend.rememberAnnotation(Object.assign({}, note, {invalid:true}));
                    applicationWindow().showPassiveNotification(i18n("The remark's EPUB anchor is missing."));
                }
                break;
            }
            case 'annotation-location':
                if (action.payload.bookId && action.payload.bookId !== root.bookServerIdentifier()) {
                    break;
                }
                backend.updateAnnotationRuntimeLocation(action.payload);
                break;
            case 'show-selection':
                if (action.payload.bookId && action.payload.bookId !== root.bookServerIdentifier()) break;
                if (action.payload.type !== 'reader-targets') backend.readerContextSelection = JSON.parse(JSON.stringify(action.payload));
                backend.selection = action.payload;
                break;
            case 'show-image-context-menu':
                mathWindow.cancelContextMenu();
                imageContextMenu.formulaContext = false;
                imageContextMenu.nativeImageActions = false;
                imageContextMenu.popup();
                break;
            case 'reference-overview':
                if (action.payload.bookId && action.payload.bookId !== root.bookServerIdentifier()) {
                    break;
                }
                backend.referenceOverview = action.payload.references || [];
                backend.updateReferenceOverviewModel(backend.referenceOverview);
                break;
            case 'relocate':
                root.speechSelection = [];
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
                const initialSearch = root.initialSearch;
                if (initialSearch && action.payload.query === initialSearch.query && action.payload.targetCfi === initialSearch.location) {
                    root.initialSearch = null;
                    if (action.payload.selectedResult >= 0) {
                        searchResultModel.selectResult(action.payload.selectedResult);
                    } else {
                        view.goTo(initialSearch.location);
                    }
                }
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
                image.not_inverse,
                .not_inverse svg,
                .not_inverse img,
                .not_inverse image {
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
                image.not_inverse,
                .not_inverse svg,
                .not_inverse img,
                .not_inverse image {
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
                :is(a, span)[data-role="anchor"][data-anchor-type="crossref"],
                a[data-role="anchor"][href]:not([data-anchor-type="crossref"]) {
                    border-bottom: 1px dashed currentColor;
                    color: inherit;
                    text-decoration: none;
                    cursor: pointer;
                }
                .bookref:hover,
                :is(a, span)[data-role="anchor"][data-anchor-type="crossref"]:hover,
                a[data-role="anchor"][href]:not([data-anchor-type="crossref"]):hover {
                    border-bottom-style: solid;
                }
                .bookref::before,
                :is(a, span)[data-role="anchor"][data-anchor-type="crossref"]::before,
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
                :is(a, span)[data-role="anchor"][data-anchor-type="crossref"]:hover::before,
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
        sequence: "Num++"
        enabled: root.visible && !root.readerSuspended && (view.activeFocus || focusScope.activeFocus)
        onActivated: root.changePageWidth(25)
    }

    Shortcut {
        sequence: "Num+-"
        enabled: root.visible && !root.readerSuspended && (view.activeFocus || focusScope.activeFocus)
        onActivated: root.changePageWidth(-25)
    }

    Shortcut {
        sequence: "Ctrl+E"
        enabled: root.canEditBook()
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
