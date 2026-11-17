// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.arianna
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.delegates as Delegates

Kirigami.Page {
    id: root

    property string url: ""
    property string filename: ""
    property string locations: ""
    property string currentLocation: ""
    property real zoomLevel: 1.0
    property string pageMode: ""
    property int pageWidthDistance: root.entry && root.entry.pageWidthDistance >= 0 ? root.entry.pageWidthDistance : -1
    readonly property int effectivePageSpacing: root.pageWidthDistance >= 0 ? root.pageWidthDistance : 4 * Kirigami.Units.largeSpacing
    property string pdfNotInvertedRegions: ""
    property var entry: null
    property bool readOnly: false
    property string referenceSourceTitle: ""
    property bool initialLocationApplied: false
    property bool restoringInitialLocation: false
    property string searchText: ""
    property var initialSearch: null
    property string speechSelectionText: ""
    property var speechSelectionPosition: ({})
    property bool continuousSpeech: false
    property int speechMode: 0
    property bool finiteSpeechSentence: false
    property int speechStreamId: 0
    property int speechPage: 0
    property int speechOffset: 0
    property bool speechBatchPending: false

    function cancelContinuousSpeech() {
        continuousSpeech = false;
        speechStreamId++;
        speechBatchPending = false;
    }

    function stopSpeech() {
        cancelContinuousSpeech();
        speech.stop();
    }

    function readSpeechSelection(text) {
        if (speechMode === 2 && speechSelectionPosition.page !== undefined) {
            readDocumentFrom(speechSelectionPosition);
            return;
        }
        cancelContinuousSpeech();
        speechControls.open();
        speech.speakSegments([{text: text.replace(/\s+/g, " ").trim(), page: speechSelectionPosition.page,
            endPage: speechSelectionPosition.endPage, sourceEnd: speechSelectionPosition.endOffset}]);
    }

    function readDocumentFrom(position) {
        if (!document.opened || position.page === undefined) return;
        speech.stop();
        cancelContinuousSpeech();
        continuousSpeech = true;
        speechMode = 2;
        speech.repeatSelection = false;
        speechPage = position.page;
        speechOffset = position.offset;
        speechControls.open();
        requestSpeechBatch(speech.sessionId, true);
    }

    function requestSpeechBatch(sessionId, initial) {
        if (!continuousSpeech || speechBatchPending) return;
        speechBatchPending = true;
        const streamId = speechStreamId;
        // Each page yields to the event loop, including empty/scanned pages.
        // Closing or replacing the book invalidates every outstanding refill.
        Qt.callLater(() => root.loadSpeechBatch(streamId, sessionId, initial));
    }

    function loadSpeechBatch(streamId, sessionId, initial) {
        if (!continuousSpeech || streamId !== speechStreamId || sessionId !== speech.sessionId) return;
        const batch = speechPage < document.pageCount ? document.speechPageSegments(speechPage, speechOffset) : {};
        const segments = batch.segments || [];
        if (batch.endOfPage || !batch.segments) {
            speechPage++;
            speechOffset = 0;
        } else {
            speechOffset = batch.nextOffset;
        }
        const hasMore = speechPage < document.pageCount;
        speechBatchPending = false;
        if (!segments.length && hasMore) {
            requestSpeechBatch(sessionId, initial);
        } else if (initial) {
            if (segments.length) speech.startReading(segments, hasMore);
            else cancelContinuousSpeech();
        } else {
            speech.continueReading(sessionId, segments, hasMore);
        }
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
    property string activeSearchText: ""
    property var headerSearchField: null
    property bool twoPageMode: false
    property bool notInvertedRegionSelectionMode: false
    property bool ovalNotInvertedRegionSelectionMode: false
    property bool forcedInvertedRegionSelectionMode: false
    property int translationRequestId: 0
    property int currentReaderTheme: Config.readerTheme
    property int pdfNotInvertedRegionsVersion: 0
    property var pdfNotInvertedRegionData: ({
            pages: ({})
        })
    property var pendingNotInvertedRegion: null
    property var pendingForcedInvertedRegion: null
    // A PDF link is document-owned and exists only after its target is chosen
    // and PoDoFo has written the native annotation. Personal notes never use
    // this state or the PDF write path.
    property var pendingPdfLinkSource: null
    // A PDF source anchor is persisted in the document. Its target remains in
    // ReferenceStore, just like EPUB cross references.
    property var pendingExternalBookReferenceSource: null
    property var editingExternalBookReference: null
    property int externalReferenceDialogGeneration: 0
    property int externalReferenceLocationGeneration: 0
    property bool externalReferenceBooksLoading: false
    property bool externalReferenceLocationsLoading: false
    property string externalReferenceError: ""
    property bool pdfLinkTargetSelectionMode: false
    property var pendingPersonalPdfNote: null
    property var editingPersonalPdfAnnotation: null
    property var personalPdfAnnotations: []
    property string currentPersonalPdfAnnotationId: ""
    readonly property string personalPdfAnnotationBookId: root.personalAnnotationBookId()
    property var pdfAriannaAnchors: []
    property var tableOfContentsEntries: []
    property int manualTableOfContentsPage: -1
    property var manualTableOfContentsTargets: []
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
    readonly property bool activeLayerPage: applicationWindow() ? applicationWindow().pageStack.layers.currentItem === root : false
    onActiveLayerPageChanged: if (!activeLayerPage) root.stopSpeech()
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
    signal pageWidthDistanceSaved(value: int)
    signal pdfNotInvertedRegionsSaved(regions: string)
    signal bookReady(title: var)
    signal bookClosed
    signal addToLibraryRequested(var callback)

    title: root.displayTitle + (root.referenceSourceTitle ? " <- " + root.referenceSourceTitle : "")
    padding: 0

    function executePdfSearch() {
        root.initialSearch = null;
        root.activeSearchText = root.searchText;
        if (root.activeSearchText.length > 0) {
            view.openSearchDialog();
        } else if (root.headerSearchField) {
            root.headerSearchField.forceActiveFocus();
        }
    }

    function applyInitialSearch() {
        if (!root.initialSearch || !document.opened || !root.initialLocationApplied) {
            return;
        }
        const initialSearch = root.initialSearch;
        root.initialSearch = null;
        root.activeSearchText = initialSearch.query;
        // Let the search binding refresh the matches before selecting the requested occurrence.
        Qt.callLater(() => {
            if (root.activeSearchText !== initialSearch.query) {
                return;
            }
            const page = root.pageFromLocation(initialSearch.location);
            let occurrence = 0;
            const matches = view.searchModel.matches;
            for (let index = 0; index < matches.length; ++index) {
                if (matches[index].page === page && occurrence++ === initialSearch.indexAtLocation) {
                    view.goToSearchResult(index);
                    return;
                }
            }
            // Text extraction can differ between the library and Okular. Keep the requested page.
            view.searchModel.currentResult = -1;
            root.goToPage(page);
        });
    }

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

    function changePageSpacing(delta) {
        if (!root.twoPageMode) {
            return;
        }
        root.pageWidthDistance = Math.max(0, Math.round(root.effectivePageSpacing + delta));
        root.pageWidthDistanceSaved(root.pageWidthDistance);
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

    function personalAnnotationBookId() {
        const uniqueIdentifier = root.entry && root.entry.uniqueIdentifier ? String(root.entry.uniqueIdentifier) : "";
        return uniqueIdentifier.length > 0 ? uniqueIdentifier : String(root.filename || "");
    }

    function bookServerIdentifier() {
        return root.entry ? String(root.entry.uniqueIdentifier || root.entry.identifier || "") : "";
    }

    function requestBookServerJson(path, success, failure) {
        if (!bookServerBaseUrl || !bookServerSessionToken) {
            failure(i18n("The book server is unavailable."));
            return;
        }

        let settled = false;
        const fail = message => {
            if (settled) {
                return;
            }
            settled = true;
            failure(message);
        };
        const request = new XMLHttpRequest();
        request.open("GET", String(bookServerBaseUrl) + path);
        request.setRequestHeader("X-Arianna-Session-Token", String(bookServerSessionToken));
        request.onreadystatechange = function() {
            if (request.readyState !== 4 || settled) {
                return;
            }

            if (request.status < 200 || request.status >= 300) {
                console.warn("Book server request failed", request.status, path, request.responseText);
                fail(i18n("Unable to load cross-reference data."));
                return;
            }

            try {
                const response = request.responseText ? JSON.parse(request.responseText) : {};
                settled = true;
                success(response);
            } catch (error) {
                console.warn("Book server returned invalid JSON", path, error);
                fail(i18n("Unable to load cross-reference data."));
            }
        };
        request.onerror = function() {
            console.warn("Book server network request failed", path);
            fail(i18n("Unable to load cross-reference data."));
        };
        request.onabort = function() {
            console.warn("Book server request was aborted", path);
            fail(i18n("Unable to load cross-reference data."));
        };
        request.send();
    }

    function externalReferenceAuthors(book) {
        if (book && Array.isArray(book.author)) {
            return book.author;
        }
        return book && book.author ? [book.author] : [];
    }

    function externalReferenceBookTitle(book) {
        return String(book && (book.title || book.id) || "");
    }

    function externalReferenceBookLabel(book) {
        const authors = root.externalReferenceAuthors(book).join(", ");
        const title = root.externalReferenceBookTitle(book);
        return authors.length > 0 ? title + " - " + authors : title;
    }

    function externalReferenceSharesAuthor(book, sourceBook) {
        const sourceAuthors = root.externalReferenceAuthors(sourceBook).map(author => String(author).trim().toLocaleLowerCase()).filter(Boolean);
        if (sourceAuthors.length === 0) {
            return false;
        }
        return root.externalReferenceAuthors(book).some(author => sourceAuthors.indexOf(String(author).trim().toLocaleLowerCase()) !== -1);
    }

    function externalReferenceTargetLocationLabel(location) {
        const title = String(location.title || "").trim();
        const preview = String(location.previewHtml || "").replace(/<[^>]+>/g, " ").replace(/\s+/g, " ").trim();
        const value = String(location.hrefLocation || location.readerLocation || location.location || location.id || "");
        return title.length > 0 ? title : (preview.length > 0 ? preview.substring(0, 80) : value);
    }

    function addExternalReferenceBookGroup(label, books) {
        if (books.length === 0) {
            return;
        }
        externalReferenceBookModel.append({
                bookId: "",
                title: label,
                selectable: false,
                category: true
            });
        for (let index = 0; index < books.length; ++index) {
            const book = books[index];
            externalReferenceBookModel.append({
                    bookId: String(book.id),
                    title: root.externalReferenceBookLabel(book),
                    selectable: true,
                    category: false
                });
        }
    }

    function populateExternalReferenceBooks(books, references) {
        const sourceBookId = root.bookServerIdentifier();
        const booksById = {};
        const inputBooks = Array.isArray(books) ? books : [];
        for (let index = 0; index < inputBooks.length; ++index) {
            const book = inputBooks[index];
            if (book && book.id) {
                booksById[String(book.id)] = book;
            }
        }
        if (sourceBookId.length > 0 && !booksById[sourceBookId]) {
            booksById[sourceBookId] = { id: sourceBookId, title: sourceBookId, author: [] };
        }
        const preferred = root.editingExternalBookReference;
        if (preferred && preferred.targetBookId && !booksById[preferred.targetBookId]) {
            booksById[preferred.targetBookId] = { id: preferred.targetBookId, title: preferred.targetBookId, author: [] };
        }

        const allBooks = Object.keys(booksById).map(bookId => booksById[bookId]);
        allBooks.sort((left, right) => root.externalReferenceBookTitle(left).localeCompare(root.externalReferenceBookTitle(right), undefined, {
                    sensitivity: "base"
                }));
        const ownBook = sourceBookId.length > 0 ? booksById[sourceBookId] : null;
        const referencedBookIds = {};
        const inputReferences = Array.isArray(references) ? references : [];
        for (let index = 0; index < inputReferences.length; ++index) {
            const targetBookId = String(inputReferences[index].targetBookId || "");
            if (targetBookId.length > 0 && targetBookId !== sourceBookId) {
                referencedBookIds[targetBookId] = true;
            }
        }

        const externallyReferencedBooks = allBooks.filter(book => book.id !== sourceBookId && referencedBookIds[String(book.id)]);
        const sameAuthorBooks = allBooks.filter(book => book.id !== sourceBookId
                                                       && !referencedBookIds[String(book.id)]
                                                       && root.externalReferenceSharesAuthor(book, ownBook));
        const otherBooks = allBooks.filter(book => book.id !== sourceBookId
                                                  && !referencedBookIds[String(book.id)]
                                                  && !root.externalReferenceSharesAuthor(book, ownBook));

        externalReferenceBookModel.clear();
        externalReferenceBookModel.append({
                bookId: "",
                title: i18n("-- Please choose --"),
                selectable: true,
                category: false
            });
        root.addExternalReferenceBookGroup(i18n("This Book"), ownBook ? [ownBook] : []);
        root.addExternalReferenceBookGroup(i18n("Already Externally Referenced Books"), externallyReferencedBooks);
        root.addExternalReferenceBookGroup(i18n("Books by the Same Author"), sameAuthorBooks);
        root.addExternalReferenceBookGroup(i18n("Other Books"), otherBooks);
        externalReferenceBookCombo.currentIndex = 0;
        if (preferred) {
            for (let index = 1; index < externalReferenceBookModel.count; ++index) {
                if (externalReferenceBookModel.get(index).bookId === preferred.targetBookId) {
                    externalReferenceBookCombo.currentIndex = index;
                    root.loadExternalReferenceTargetLocations(preferred);
                    break;
                }
            }
        }
    }

    function clearExternalReferenceTargetLocations() {
        externalReferenceTargetLocationModel.clear();
        externalReferenceTargetLocationModel.append({
                targetLocation: "",
                previewHtml: "",
                title: i18n("-- No selection --")
            });
        externalReferenceTargetLocationCombo.currentIndex = 0;
        externalReferenceTargetLocationInput.text = "";
        externalReferenceTargetLocationInput.edited = false;
        externalReferencePreviewHtml.text = "";
        externalReferencePreviewHtml.edited = false;
    }

    function selectedExternalReferenceBook() {
        if (externalReferenceBookCombo.currentIndex < 0) {
            return null;
        }
        const entry = externalReferenceBookModel.get(externalReferenceBookCombo.currentIndex);
        return entry && entry.selectable && entry.bookId.length > 0 ? entry : null;
    }

    function selectedExternalReferenceTargetLocation() {
        if (externalReferenceTargetLocationCombo.currentIndex < 0) {
            return null;
        }
        const entry = externalReferenceTargetLocationModel.get(externalReferenceTargetLocationCombo.currentIndex);
        return entry && entry.targetLocation.length > 0 ? entry : null;
    }

    function selectStoredExternalReferenceTarget(preferred) {
        if (!preferred || !preferred.targetLocation || externalReferenceTargetLocationInput.edited) {
            return;
        }
        let selectedIndex = -1;
        for (let index = 1; index < externalReferenceTargetLocationModel.count; ++index) {
            if (externalReferenceTargetLocationModel.get(index).targetLocation === preferred.targetLocation) {
                selectedIndex = index;
                break;
            }
        }
        // A saved CFI, PDF rectangle or custom anchor may not be in the TOC.
        // Keep its exact location and the user's preview when editing.
        if (selectedIndex < 0) {
            selectedIndex = externalReferenceTargetLocationModel.count;
            externalReferenceTargetLocationModel.append({
                    targetLocation: String(preferred.targetLocation),
                    previewHtml: String(preferred.targetPreviewHtml || ""),
                    title: i18n("Current target location")
                });
        } else {
            externalReferenceTargetLocationModel.setProperty(selectedIndex, "previewHtml", String(preferred.targetPreviewHtml || ""));
        }
        externalReferenceTargetLocationCombo.currentIndex = selectedIndex;
        root.updateExternalReferencePreview(!externalReferencePreviewHtml.edited);
    }

    function loadExternalReferenceTargetLocations(preferred) {
        const generation = ++root.externalReferenceLocationGeneration;
        const book = root.selectedExternalReferenceBook();
        root.clearExternalReferenceTargetLocations();
        root.externalReferenceLocationsLoading = false;
        if (!book) {
            return;
        }
        if (preferred) {
            externalReferenceTargetLocationInput.text = String(preferred.targetLocation || "");
            externalReferencePreviewHtml.text = String(preferred.targetPreviewHtml || "");
            externalReferencePreviewHtml.edited = false;
        }

        root.externalReferenceLocationsLoading = true;
        // The native dialog only needs navigable TOC entries. Avoid parsing
        // every XHTML anchor of a large target EPUB on the GUI thread.
        root.requestBookServerJson("/" + encodeURIComponent(book.bookId) + "/targetLocations?tocOnly=1", data => {
            if (generation !== root.externalReferenceLocationGeneration) {
                return;
            }
            root.externalReferenceLocationsLoading = false;
            const locations = Array.isArray(data) ? data : (Array.isArray(data.targetLocations) ? data.targetLocations : []);
            for (let index = 0; index < locations.length; ++index) {
                const location = locations[index];
                const targetLocation = String(location.readerLocation || location.location || "");
                if (targetLocation.length === 0) {
                    continue;
                }
                externalReferenceTargetLocationModel.append({
                        targetLocation: targetLocation,
                        previewHtml: String(location.previewHtml || ""),
                        title: root.externalReferenceTargetLocationLabel(location)
                    });
            }
            root.selectStoredExternalReferenceTarget(preferred);
        }, error => {
            if (generation !== root.externalReferenceLocationGeneration) {
                return;
            }
            root.externalReferenceLocationsLoading = false;
            root.externalReferenceError = error;
            root.selectStoredExternalReferenceTarget(preferred);
        });
    }

    function updateExternalReferencePreview(updatePreview = true) {
        const location = root.selectedExternalReferenceTargetLocation();
        externalReferenceTargetLocationInput.text = location ? location.targetLocation : "";
        externalReferenceTargetLocationInput.edited = false;
        if (updatePreview) {
            externalReferencePreviewHtml.text = location ? location.previewHtml : "";
            externalReferencePreviewHtml.edited = false;
        }
    }

    function openExternalBookReferenceDialog(sourceSelection) {
        if (root.readOnly || !sourceSelection || !sourceSelection.rects || sourceSelection.rects.length === 0 || root.bookServerIdentifier().length === 0) {
            return;
        }
        if (!document.pdfLinkAnnotationAllowed) {
            pdfLinkPermissionDialog.open();
            return;
        }

        root.pendingExternalBookReferenceSource = sourceSelection;
        root.editingExternalBookReference = null;
        root.prepareExternalBookReferenceDialog(String(sourceSelection.text || "").replace(/\s+/g, " ").trim());
    }

    function editPdfAriannaAnchor(anchorId) {
        if (root.readOnly) {
            return;
        }
        const reference = ReferenceStore.loadReferenceBySource(root.bookServerIdentifier(), anchorId);
        if (!reference || !reference.sourceAnchorId) {
            return;
        }
        pdfReferenceDialog.close();
        root.pendingExternalBookReferenceSource = null;
        root.editingExternalBookReference = reference;
        root.prepareExternalBookReferenceDialog(String(reference.sourceAnchorTitle || ""));
    }

    function prepareExternalBookReferenceDialog(sourceTitle) {
        const generation = ++root.externalReferenceDialogGeneration;
        ++root.externalReferenceLocationGeneration;
        root.externalReferenceLocationsLoading = false;
        root.externalReferenceError = "";
        externalReferenceSourceAnchorTitle.text = sourceTitle;
        root.clearExternalReferenceTargetLocations();
        externalReferenceBookModel.clear();
        externalReferenceBookModel.append({
                bookId: "",
                title: i18n("Loading…"),
                selectable: false,
                category: false
            });
        externalReferenceBookCombo.currentIndex = 0;
        root.externalReferenceBooksLoading = true;
        externalBookReferenceDialog.open();

        root.requestBookServerJson("/books", bookData => {
            if (generation !== root.externalReferenceDialogGeneration) {
                return;
            }
            const books = Array.isArray(bookData) ? bookData : (Array.isArray(bookData.books) ? bookData.books : []);
            root.requestBookServerJson("/" + encodeURIComponent(root.bookServerIdentifier()) + "/refs", referenceData => {
                if (generation !== root.externalReferenceDialogGeneration) {
                    return;
                }
                root.externalReferenceBooksLoading = false;
                root.populateExternalReferenceBooks(books, referenceData.refs || []);
            }, error => {
                if (generation !== root.externalReferenceDialogGeneration) {
                    return;
                }
                root.externalReferenceBooksLoading = false;
                root.externalReferenceError = error;
                root.populateExternalReferenceBooks(books, []);
            });
        }, error => {
            if (generation !== root.externalReferenceDialogGeneration) {
                return;
            }
            root.externalReferenceBooksLoading = false;
            root.externalReferenceError = error;
            root.populateExternalReferenceBooks([], []);
        });
    }

    function saveExternalBookReference() {
        const sourceSelection = root.pendingExternalBookReferenceSource;
        const editingReference = root.editingExternalBookReference;
        const targetBook = root.selectedExternalReferenceBook();
        const targetLocation = externalReferenceTargetLocationInput.text.trim();
        if (root.readOnly || root.externalReferenceBooksLoading
            || (!sourceSelection && !editingReference) || !targetBook || targetLocation.length === 0) {
            return;
        }

        let anchorId = editingReference ? String(editingReference.sourceAnchorId) : "";
        if (!editingReference) {
            const result = document.createPdfAnchorAnnotation(sourceSelection, "", root.filename);
            if (!result || !result.success || !result.anchorId) {
                root.externalReferenceError = result && result.message ? result.message : i18n("Unable to write the PDF anchor.");
                return;
            }
            anchorId = String(result.anchorId);
        }

        const reference = {
                sourceBookId: root.bookServerIdentifier(),
                sourceAnchorId: anchorId,
                sourceAnchorTitle: externalReferenceSourceAnchorTitle.text.trim(),
                targetBookId: targetBook.bookId,
                targetLocation: targetLocation,
                targetPreviewHtml: externalReferencePreviewHtml.text,
                modified: editingReference ? new Date().toISOString() : ""
            };
        ReferenceStore.saveReference(reference);
        const saved = ReferenceStore.loadReferenceBySource(reference.sourceBookId, anchorId);
        if (!saved || saved.sourceAnchorTitle !== reference.sourceAnchorTitle || saved.targetBookId !== reference.targetBookId
            || saved.targetLocation !== reference.targetLocation || saved.targetPreviewHtml !== reference.targetPreviewHtml) {
            root.externalReferenceError = i18n("Unable to save the reference.");
            return;
        }
        root.pendingExternalBookReferenceSource = null;
        externalBookReferenceDialog.close();
        view.clearSelection();
        view.clearAriannaAnchorHover();
        if (editingReference) {
            root.loadPdfAriannaAnchors();
            root.openPdfAriannaAnchor(anchorId);
        } else {
            root.reloadPdfRestoringPage(sourceSelection.page);
        }
    }

    function startPdfLinkTargetSelection(sourceSelection) {
        if (root.readOnly || !sourceSelection || !sourceSelection.rects || sourceSelection.rects.length === 0) {
            return;
        }
        if (!document.pdfLinkAnnotationAllowed) {
            pdfLinkPermissionDialog.open();
            return;
        }

        root.pendingPdfLinkSource = sourceSelection;
        root.pdfLinkTargetSelectionMode = true;
    }

    function cancelPdfLinkTargetSelection() {
        root.pendingPdfLinkSource = null;
        root.pdfLinkTargetSelectionMode = false;
        if (view) {
            view.clearSelection();
        }
    }

    function reloadPdfRestoringPage(page) {
        const requestedPage = Number(page);
        if (isFinite(requestedPage) && requestedPage >= 0) {
            root.currentLocation = root.locationForPage(Math.floor(requestedPage));
            root.pdfHistoryCurrentLocation = root.currentLocation;
        } else {
            root.updateReadingPosition();
        }
        root.initialLocationApplied = false;
        const sourceBeforeReload = document.source.toString();
        pdfIoLoader.reload();
        // Reloading the source is necessary for a watermark-filtered PDF. If
        // the loader keeps the same URL, explicitly reload the document too.
        if (!pdfIoLoader.loading && document.source.toString() === sourceBeforeReload) {
            document.reload();
        }
    }

    function completePdfLinkTargetSelection(targetSelection) {
        const sourceSelection = root.pendingPdfLinkSource;
        if (!sourceSelection || !targetSelection || !targetSelection.rects || targetSelection.rects.length === 0) {
            root.cancelPdfLinkTargetSelection();
            return;
        }

        // Clearing the transient state first means an error or a cancellation
        // can never leave an in-memory half-link behind.
        root.pendingPdfLinkSource = null;
        root.pdfLinkTargetSelectionMode = false;
        const result = document.createPdfLinkAnnotation(sourceSelection, targetSelection, root.filename);
        view.clearSelection();
        if (!result || !result.success) {
            console.warn(result && result.message ? result.message : "Unable to create PDF link.");
            return;
        }

        root.reloadPdfRestoringPage(sourceSelection.page);
    }

    function openPersonalPdfNote(selection) {
        if (root.readOnly || !selection || !selection.rects || selection.rects.length === 0 || root.personalAnnotationBookId().length === 0) {
            return;
        }

        root.editingPersonalPdfAnnotation = null;
        root.pendingPersonalPdfNote = selection;
        personalPdfNoteExcerpt.text = String(selection.text || "");
        personalPdfNoteText.text = "";
        personalPdfNoteColors.selectedColor = "yellow";
        personalPdfNoteDialog.open();
        personalPdfNoteText.forceActiveFocus();
    }

    function editPersonalPdfAnnotation(annotationId) {
        const annotation = root.personalPdfAnnotations.find(item => item.annotationId === annotationId);
        if (root.readOnly || !annotation) {
            return;
        }

        root.editingPersonalPdfAnnotation = annotation;
        root.pendingPersonalPdfNote = annotation.location;
        personalPdfNoteExcerpt.text = String(annotation.text || "");
        personalPdfNoteText.text = String(annotation.note || "");
        personalPdfNoteColors.selectedColor = annotation.color || "yellow";
        if (personalPdfNotesDialog.visible) {
            const showEditor = () => {
                personalPdfNotesDialog.closed.disconnect(showEditor);
                if (!root.readOnly && root.editingPersonalPdfAnnotation?.annotationId === annotationId)
                    personalPdfNoteDialog.open();
            };
            personalPdfNotesDialog.closed.connect(showEditor);
            personalPdfNotesDialog.close();
        } else personalPdfNoteDialog.open();
    }

    function removePersonalPdfAnnotation(annotationId) {
        if (root.readOnly || !annotationId || root.personalPdfAnnotationBookId.length === 0) {
            return;
        }

        AnnotationStore.removeAnnotation(root.personalPdfAnnotationBookId, annotationId);
    }

    function navigatePersonalPdfAnnotation(previous) {
        const notes = root.personalPdfAnnotations.filter(annotation => {
            const page = annotation && annotation.location ? Number(annotation.location.page) : -1;
            return annotation && !annotation.invalid && page >= 0 && page < document.pageCount;
        }).sort((a, b) => {
            const first = a.location.rects && a.location.rects.length ? a.location.rects[0] : {};
            const second = b.location.rects && b.location.rects.length ? b.location.rects[0] : {};
            return Number(a.location.page) - Number(b.location.page)
                || (Number(first.y) || 0) - (Number(second.y) || 0)
                || (Number(first.x) || 0) - (Number(second.x) || 0);
        });
        if (notes.length === 0) {
            return;
        }
        let index = notes.findIndex(annotation => annotation.annotationId === root.currentPersonalPdfAnnotationId);
        if (index >= 0) {
            index = (index + (previous ? -1 : 1) + notes.length) % notes.length;
        } else if (previous) {
            index = notes.length - 1;
            for (let i = notes.length - 1; i >= 0; --i) {
                if (Number(notes[i].location.page) <= root.readingPage) {
                    index = i;
                    break;
                }
            }
        } else {
            index = notes.findIndex(annotation => Number(annotation.location.page) >= root.readingPage);
            if (index < 0) index = 0;
        }
        root.goToPersonalPdfAnnotation(notes[index]);
    }

    function goToPersonalPdfAnnotation(annotation) {
        if (!annotation || !annotation.location) {
            return;
        }

        root.currentPersonalPdfAnnotationId = annotation.annotationId || "";
        const location = annotation.location;
        const rect = location.rects && location.rects.length > 0 ? location.rects[0] : null;
        root.followPdfLink(JSON.stringify({
            format: "pdf",
            page: Number(location.page),
            x: rect ? Number(rect.x) : 0,
            y: rect ? Number(rect.y) : 0,
            position: "topLeft"
        }), root.currentPdfHistoryLocation());
        personalPdfNotesDialog.close();
    }

    function loadPersonalPdfAnnotations() {
        const bookId = root.personalAnnotationBookId();
        if (bookId.length === 0) {
            root.personalPdfAnnotations = [];
            return;
        }

        const annotations = AnnotationStore.loadAnnotations(bookId);
        root.personalPdfAnnotations = annotations.filter(annotation => annotation && annotation.format === "pdf" && annotation.location);
    }

    function loadPdfAriannaAnchors() {
        if (!document.opened || root.filename.length === 0) {
            root.pdfAriannaAnchors = [];
            return;
        }

        const sourceBookId = root.bookServerIdentifier();
        const references = sourceBookId.length > 0 ? ReferenceStore.loadReferences(sourceBookId) : [];
        if (references.length === 0) {
            root.pdfAriannaAnchors = [];
            return;
        }
        const anchors = document.pdfAriannaAnchors(root.filename);
        const referencesByAnchorId = {};
        for (let index = 0; index < references.length; ++index) {
            const reference = references[index];
            if (reference && reference.sourceAnchorId) {
                referencesByAnchorId[String(reference.sourceAnchorId)] = reference;
            }
        }
        const anchorsWithReferenceTitles = [];
        for (let index = 0; index < anchors.length; ++index) {
            const anchor = anchors[index];
            const anchorId = String(anchor.anchorId || "");
            const reference = referencesByAnchorId[anchorId];
            anchorsWithReferenceTitles.push({
                    anchorId: anchorId,
                    page: Number(anchor.page),
                    rects: anchor.rects || [],
                    sourceAnchorTitle: reference ? String(reference.sourceAnchorTitle || "") : "",
                    hasReference: reference !== undefined
                });
        }
        root.pdfAriannaAnchors = anchorsWithReferenceTitles;
    }

    function openPdfAriannaAnchor(anchorId) {
        const reference = ReferenceStore.loadReferenceBySource(root.bookServerIdentifier(), anchorId);
        if (!reference || !reference.sourceAnchorId) {
            return;
        }
        view.clearAriannaAnchorHover();
        pdfReferenceDialog.reference = reference;
        pdfReferenceDialog.error = "";
        pdfReferenceDialog.openingTarget = false;
        pdfReferenceDialog.open();
    }

    function goToPdfAriannaAnchor(anchor) {
        if (!anchor) {
            return;
        }

        const rect = anchor.rects && anchor.rects.length > 0 ? anchor.rects[0] : null;
        root.followPdfLink(JSON.stringify({
            format: "pdf",
            page: Number(anchor.page),
            x: rect ? Number(rect.x) : 0,
            y: rect ? Number(rect.y) : 0,
            position: "topLeft"
        }), root.currentPdfHistoryLocation());
        pdfReferencesDialog.close();
    }

    function followPdfAriannaAnchor(anchorId) {
        const sourceBookId = root.bookServerIdentifier();
        if (sourceBookId.length === 0 || !anchorId) {
            return;
        }

        pdfReferenceDialog.openingTarget = true;
        pdfReferenceDialog.error = "";
        const displayedReference = pdfReferenceDialog.reference;
        root.requestBookServerJson("/" + encodeURIComponent(sourceBookId) + "/ref/" + encodeURIComponent(anchorId), reference => {
            if (!pdfReferenceDialog.visible || pdfReferenceDialog.reference !== displayedReference) {
                return;
            }
            pdfReferenceDialog.openingTarget = false;
            const target = reference && reference.target ? reference.target : {};
            const targetEntry = target.entry || {};
            const targetFilename = String(targetEntry.filename || "");
            const targetLocation = String(reference.targetLocation || target.location || "");
            if (targetFilename.length === 0 || targetLocation.length === 0) {
                pdfReferenceDialog.error = i18n("Unable to load cross-reference data.");
                return;
            }

            if (typeof applicationWindow().openReferencePage === "function") {
                pdfReferenceDialog.close();
                applicationWindow().openReferencePage(targetFilename,
                                                      targetLocation,
                                                      targetEntry,
                                                      target.readOnly === true,
                                                      String(targetEntry.locations || ""),
                                                      root.displayTitle);
            }
        }, error => {
            if (pdfReferenceDialog.reference === displayedReference) {
                pdfReferenceDialog.openingTarget = false;
                pdfReferenceDialog.error = error;
            }
        });
    }

    function savePersonalPdfNote() {
        const selection = root.pendingPersonalPdfNote;
        if (root.readOnly || !selection || root.personalPdfAnnotationBookId.length === 0) {
            return;
        }

        const existing = root.editingPersonalPdfAnnotation;
        const annotation = existing ? Object.assign({}, existing) : {
            format: "pdf",
            location: selection,
            color: "yellow",
            text: String(selection.text || ""),
            created: new Date().toISOString(),
            modified: ""
        };
        annotation.note = personalPdfNoteText.text;
        annotation.color = personalPdfNoteColors.selectedColor;
        if (existing) {
            annotation.modified = new Date().toISOString();
        }
        const result = AnnotationStore.createUserAnnotation(root.personalPdfAnnotationBookId, annotation);
        if (!result || result.error) {
            console.warn(result && result.message ? result.message : "Unable to save PDF note.");
        }
        root.pendingPersonalPdfNote = null;
        root.editingPersonalPdfAnnotation = null;
        view.clearSelection();
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
        drawer.clearCurrentItem();
        const tableOfContentsJson = document.tableOfContentsJson || "[]";
        drawer.model.importFromJson(tableOfContentsJson);
        root.primaryTableOfContentsEntries = root.primaryEntriesFromTableOfContents(tableOfContentsJson);
        root.primaryTableOfContentsPages = root.primaryTableOfContentsEntries.map(entry => entry.page);
        root.tableOfContentsEntries = root.flattenTableOfContents(tableOfContentsJson);
        root.updateCurrentTableOfContentsItem(root.readingPage);
    }

    function manualTableOfContentsTargetsForDialog(page) {
        const targets = [];
        const requestedPage = root.boundedDocumentPage(page);
        let previousTargetIndex = -1;
        let previousTargetPage = -1;
        const appendTargets = (entries, depth) => {
            if (!Array.isArray(entries)) {
                return;
            }

            for (const entry of entries) {
                const item = entry || {};
                const id = String(item.id || "");
                if (id.length > 0) {
                    const indentation = "\u00a0\u00a0".repeat(depth);
                    targets.push({
                        id: id,
                        title: indentation + String(item.label || item.href || ""),
                        isAppendTarget: false,
                        defaultTarget: false
                    });
                    const targetPage = root.pageFromLocation(item.href || "");
                    if (isFinite(targetPage) && targetPage <= requestedPage && targetPage >= previousTargetPage) {
                        previousTargetIndex = targets.length - 1;
                        previousTargetPage = targetPage;
                    }
                }
                appendTargets(item.subitems, depth + 1);
            }
        };

        try {
            appendTargets(JSON.parse(document.tableOfContentsJson || "[]"), 0);
        } catch (error) {}
        if (previousTargetIndex >= 0) {
            targets[previousTargetIndex].defaultTarget = true;
        }
        targets.push({
            id: "",
            title: i18nc("@item:inlistbox", "End of table of contents"),
            isAppendTarget: true,
            defaultTarget: previousTargetIndex < 0
        });
        return targets;
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
        manualTableOfContentsTargets = root.manualTableOfContentsTargetsForDialog(manualTableOfContentsPage);
        const defaultTargetIndex = manualTableOfContentsTargets.findIndex(target => target.defaultTarget);
        manualTableOfContentsTarget.currentIndex = defaultTargetIndex >= 0 ? defaultTargetIndex : manualTableOfContentsTargets.length - 1;
        manualTableOfContentsPlacement.currentIndex = 1;
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

        const page = root.boundedDocumentPage(manualTableOfContentsPage);
        const target = manualTableOfContentsTarget.currentIndex >= 0 ? root.manualTableOfContentsTargets[manualTableOfContentsTarget.currentIndex] : null;
        const targetId = target && target.id ? String(target.id) : "";
        const entry = {
            label: title,
            id: "pdf-manual-toc-" + page,
            href: root.locationForPage(page),
            subitems: [],
            targetId: targetId,
            placement: targetId.length > 0 ? String(manualTableOfContentsPlacement.currentValue) : "append"
        };
        const sourceBeforeWrite = document.source;
        document.source = Qt.url("");
        const result = document.writeTableOfContentsToPdf(JSON.stringify([entry]), root.filename);
        if (!result || !result.success) {
            document.source = sourceBeforeWrite;
            console.warn(result && result.message ? result.message : "Unable to write PDF table of contents.");
            return;
        }
        document.source = sourceBeforeWrite;

        root.reloadPdfRestoringPage(page);
    }

    function removeTableOfContentsItem(tocId) {
        const result = document.removeTableOfContentsItemFromPdf(tocId, root.filename);
        if (!result || !result.success) {
            console.warn(result && result.message ? result.message : "Unable to remove PDF table of contents item.");
            return;
        }

        root.reloadPdfRestoringPage();
    }

    function prepareTableOfContentsItemEdit(tocId, title) {
        if (!document.opened || !tocId) {
            return;
        }

        editTableOfContentsItemDialog.tocId = tocId;
        editTableOfContentsItemTitle.text = title;
        editTableOfContentsItemDialog.open();
        editTableOfContentsItemTitle.forceActiveFocus();
        editTableOfContentsItemTitle.selectAll();
    }

    function editTableOfContentsItem() {
        const title = editTableOfContentsItemTitle.text.trim();
        if (title.length === 0) {
            return;
        }

        const result = document.editTableOfContentsItemInPdf(editTableOfContentsItemDialog.tocId, title, root.filename);
        if (!result || !result.success) {
            console.warn(result && result.message ? result.message : "Unable to edit PDF table of contents item.");
            return;
        }

        root.reloadPdfRestoringPage();
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
            pageMode: root.currentPageMode(),
            pageWidthDistance: root.pageWidthDistance
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

    function pdfEditorProgramAndArguments() {
        const command = root.pdfEditorCommand();
        const tokens = command.match(/"[^"]+"|'[^']+'|\S+/g) || [];
        const program = tokens.length > 0 ? tokens[0].replace(/^"|"$/g, "").replace(/^'|'$/g, "") : "";
        const argv = tokens.slice(1).map(token => token.replace(/^"|"$/g, "").replace(/^'|'$/g, ""));
        return {
            program,
            arguments: argv
        };
    }

    function pdfEditorPageSwitchConfigured() {
        const editor = root.pdfEditorProgramAndArguments();
        for (let i = 0; i < editor.arguments.length; ++i) {
            if (editor.arguments[i] === "--page" && i + 1 < editor.arguments.length && editor.arguments[i + 1] === "default") {
                return true;
            }
        }
        return false;
    }

    function stripPageDefaultArgument(args) {
        const filtered = [];
        for (let i = 0; i < args.length; ++i) {
            if (args[i] === "--page" && i + 1 < args.length && args[i + 1] === "default") {
                ++i;
                continue;
            }
            filtered.push(args[i]);
        }
        return filtered;
    }

    function canEditBook() {
        return root.pdfEditorCommand().length > 0 && root.filename.length > 0 && root.documentReady;
    }

    function currentEditorPageNumber(page) {
        const maximumPage = Math.max(1, document.pageCount);
        const requestedPage = Number(page);
        const currentPage = isFinite(requestedPage) && requestedPage >= 0 ? Math.floor(requestedPage) + 1 : root.activePage >= 0 ? root.activePage + 1 : 1;
        return Math.max(1, Math.min(currentPage, maximumPage));
    }

    function editBook(page) {
        if (!root.canEditBook()) {
            return;
        }

        root.updateReadingPosition();
        const editor = root.pdfEditorProgramAndArguments();
        const args = root.pdfEditorPageSwitchConfigured() ? root.stripPageDefaultArgument(editor.arguments) : editor.arguments;
        const editingArgs = args.concat(["--page", String(root.currentEditorPageNumber(page)), root.filename]);
        if (!ExternalProcess.startDetached(editor.program, editingArgs)) {
            console.warn("Unable to start PDF editor for:", root.filename);
        }
    }

    function showTranslation(text, hasTranscription = false) {
        translatorDialog.hasTranscription = hasTranscription;
        translatedText.text = text;
        translatorDialog.open();
    }

    function translateSelection(selectedText) {
        const text = String(selectedText || "").trim();
        if (text.length === 0) {
            return;
        }

        root.showTranslation(i18n("Translating..."));
        root.translationRequestId = Translator.translate(text);
    }

    function applyUpdatedBookEntry(updatedEntry) {
        if (!updatedEntry || !updatedEntry.filename || updatedEntry.filename !== root.filename) {
            return;
        }

        root.entry = updatedEntry;
        if (updatedEntry.pageWidthDistance !== undefined && updatedEntry.pageWidthDistance !== null) {
            root.pageWidthDistance = updatedEntry.pageWidthDistance;
        }
        if (updatedEntry.pdfNotInvertedRegions !== undefined) {
            root.pdfNotInvertedRegions = updatedEntry.pdfNotInvertedRegions || "";
        }
        if (updatedEntry.currentLocation) {
            root.currentLocation = updatedEntry.currentLocation;
        }
        // Saving another setting can echo an older zoom while our local zoom
        // is still waiting for its save timer. Keep that pending user change.
        if (updatedEntry.zoomLevel > 0 && !zoomSaveTimer.running) {
            root.setZoom(updatedEntry.zoomLevel);
        }
        if (updatedEntry.pageMode) {
            root.pageMode = root.normalizedPageMode(updatedEntry.pageMode);
            root.applyPageMode(root.pageMode);
        }
    }

    onUrlChanged: {
        speechControls.close();
        root.stopSpeech();
        root.speechSelectionText = "";
        root.speechSelectionPosition = ({});
        if (externalBookReferenceDialog) {
            externalBookReferenceDialog.close();
        }
        if (pdfReferenceDialog) {
            pdfReferenceDialog.close();
        }
        if (pdfReferencesDialog) {
            pdfReferencesDialog.close();
        }
        if (personalPdfNoteDialog) {
            personalPdfNoteDialog.close();
        }
        if (personalPdfNotesDialog) {
            personalPdfNotesDialog.close();
        }
        root.pendingPersonalPdfNote = null;
        root.editingPersonalPdfAnnotation = null;
        root.initialLocationApplied = false;
        root.restoringInitialLocation = false;
        root.cancelPdfLinkTargetSelection();
        root.personalPdfAnnotations = [];
        root.currentPersonalPdfAnnotationId = "";
        root.pdfAriannaAnchors = [];
        root.clearPdfHistory();
    }

    onPersonalPdfAnnotationBookIdChanged: root.loadPersonalPdfAnnotations()

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
        root.stopSpeech();
        zoomSaveTimer.flush();
        root.updateReadingPosition();
        root.bookClosed();
    }

    property list<Kirigami.Action> speechActions: [
        Kirigami.Action {
            objectName: "pdfSpeechStartAction"
            text: speech.state === SpeechController.Paused ? i18nc("@action:intoolbar", "Resume Reading") : i18nc("@action:intoolbar", "Read Selection Aloud")
            icon.name: "media-playback-start"
            displayHint: Kirigami.DisplayHint.IconOnly | Kirigami.DisplayHint.KeepVisible
            enabled: speech.state === SpeechController.Paused || (root.speechSelectionText.trim().length > 0)
            onTriggered: {
                if (speech.state === SpeechController.Paused) {
                    speech.resume();
                } else {
                    root.readSpeechSelection(root.speechSelectionText);
                }
            }
        },
        Kirigami.Action {
            objectName: "pdfSpeechStopAction"
            text: i18nc("@action:intoolbar", "Stop Reading")
            icon.name: "media-playback-stop"
            displayHint: Kirigami.DisplayHint.IconOnly | Kirigami.DisplayHint.KeepVisible
            enabled: speech.state !== SpeechController.Idle
            onTriggered: root.stopSpeech()
        },
        Kirigami.Action {
            objectName: "pdfSpeechPauseAction"
            text: i18nc("@action:intoolbar", "Pause Reading")
            icon.name: "media-playback-pause"
            displayHint: Kirigami.DisplayHint.IconOnly | Kirigami.DisplayHint.KeepVisible
            enabled: speech.state === SpeechController.Speaking
            onTriggered: speech.pause()
        },
        Kirigami.Action {
            objectName: "pdfSpeechModeAction"
            text: root.speechMode === 2 ? i18nc("@action:intoolbar", "Continuous Reading") : root.speechMode === 1 ? i18nc("@action:intoolbar", "Repeat Selection") : i18nc("@action:intoolbar", "Read Selection Once")
            icon.name: root.speechMode === 1 ? "media-playlist-repeat" : "media-playlist-normal"
            icon.source: root.speechMode === 2 ? "qrc:/qt/qml/org/kde/arianna/qml/icons/speech-continuous.svg" : ""
            displayHint: Kirigami.DisplayHint.IconOnly | Kirigami.DisplayHint.KeepVisible
            checkable: true
            checked: root.speechMode === 1
            onTriggered: root.setSpeechMode((root.speechMode + 1) % 3)
        }
    ]

    property Kirigami.Action speechRateDecreaseAction: Kirigami.Action {
        objectName: "pdfSpeechRateDecreaseAction"
        text: i18nc("@action:intoolbar", "Decrease speech rate (current: %1%)", (speech.speechRate > 0 ? "+" : "") + speech.speechRate)
        icon.name: "go-down"
        enabled: speech.speechRate > -100
        onTriggered: speech.adjustSpeechRate(-10)
    }

    property Kirigami.Action speechRateIncreaseAction: Kirigami.Action {
        objectName: "pdfSpeechRateIncreaseAction"
        text: i18nc("@action:intoolbar", "Increase speech rate (current: %1%)", (speech.speechRate > 0 ? "+" : "") + speech.speechRate)
        icon.name: "go-up"
        enabled: speech.speechRate < 100
        onTriggered: speech.adjustSpeechRate(10)
    }

    SpeechControlWindow {
        id: speechControls
        objectName: "pdfSpeechControlWindow"
        actionPrefix: "pdf"
        controlActions: root.speechActions
        rateDecreaseAction: root.speechRateDecreaseAction
        rateIncreaseAction: root.speechRateIncreaseAction
        speechRate: speech.speechRate
        readerActive: root.activeLayerPage
        transientParent: applicationWindow()
        onStopRequested: root.stopSpeech()
    }

    SpeechController {
        id: speech
        objectName: "pdfSpeechController"
        onReadingExtended: (sessionId, requestId, lastSegment) => {
            if (!root.continuousSpeech || requestId !== String(root.speechStreamId) || sessionId !== speech.sessionId) return;
            if (lastSegment.sourceEnd === undefined) {
                root.cancelContinuousSpeech();
                speech.continueReading(sessionId, [], false);
                return;
            }
            root.speechPage = lastSegment.endPage === undefined ? lastSegment.page : lastSegment.endPage;
            root.speechOffset = lastSegment.sourceEnd;
            root.requestSpeechBatch(sessionId, false);
        }
        onSessionChanged: root.finiteSpeechSentence = false
        onSegmentStarted: (sessionId, segment) => {
            if (sessionId === speech.sessionId && segment.page !== undefined
                && view.currentPage !== view.displayPage(segment.page)) view.goToPage(segment.page);
        }
        onMoreSegmentsRequested: (sessionId, preload) => root.requestSpeechBatch(sessionId, false)
        onStateChanged: {
            if (speech.state === SpeechController.Speaking && root.speechMode === 2
                && !root.continuousSpeech && root.finiteSpeechSentence) {
                root.setSpeechMode(2);
            } else if (speech.state === SpeechController.Idle) {
                root.cancelContinuousSpeech();
            }
        }
        onErrorOccurred: message => applicationWindow().showPassiveNotification(message, "long")
    }

    QQC2.ActionGroup {
        id: readerThemeActionGroup
        exclusive: true
    }

    actions: [
        Kirigami.Action {
            objectName: "pdfReferencesAction"
            text: i18n("References")
            displayHint: Kirigami.DisplayHint.IconOnly
            icon.source: "qrc:/qt/qml/org/kde/arianna/qml/icons/reference-blue.svg"
            enabled: document.opened && root.pdfAriannaAnchors.length > 0
            onTriggered: {
                root.loadPdfAriannaAnchors();
                pdfReferencesDialog.open();
            }
        },
        Kirigami.Action {
            objectName: "personalPdfNotesAction"
            text: i18n("Remarks")
            displayHint: Kirigami.DisplayHint.IconOnly
            icon.source: "qrc:/qt/qml/org/kde/arianna/qml/icons/bookmark-gold.svg"
            enabled: document.opened && root.personalPdfAnnotations.length > 0
            onTriggered: {
                root.loadPersonalPdfAnnotations();
                personalPdfNotesDialog.open();
            }
        },
        Kirigami.Action {
            id: pdfSearchAction
            text: i18nc("@action:intoolbar", "Search")
            icon.name: "system-search-symbolic"
            displayHint: Kirigami.DisplayHint.KeepVisible
            onTriggered: view.openSearchDialog()

            displayComponent: RowLayout {
                spacing: Kirigami.Units.smallSpacing

                QQC2.ToolButton {
                    action: pdfSearchAction
                    display: QQC2.AbstractButton.IconOnly
                    QQC2.ToolTip.text: pdfSearchAction.text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }

                QQC2.TextField {
                    id: headerPdfSearchField
                    objectName: "pdfHeaderSearchField"
                    Layout.minimumWidth: Kirigami.Units.gridUnit * 8
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                    selectByMouse: true
                    placeholderText: i18nc("@info:placeholder", "Search in PDF...")
                    text: root.searchText
                    onTextChanged: if (root.searchText !== text) root.searchText = text
                    onAccepted: root.executePdfSearch()
                    Component.onCompleted: root.headerSearchField = headerPdfSearchField
                    Component.onDestruction: if (root.headerSearchField === headerPdfSearchField) root.headerSearchField = null
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
            text: root.pdfEditorPageSwitchConfigured() ? i18nc("@action:intoolbar", "Edit Current Page") : i18nc("@action:intoolbar", "Edit Book")
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
        target: ReferenceStore

        function onReferencesChanged(bookId) {
            if (!bookId || bookId === root.bookServerIdentifier()) {
                root.loadPdfAriannaAnchors();
            }
        }
    }

    Connections {
        target: AnnotationStore

        function onAnnotationsChanged(bookId) {
            if (!bookId || bookId === root.personalPdfAnnotationBookId) {
                root.loadPersonalPdfAnnotations();
            }
        }
    }

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

        function onRemovePdfTableOfContentsItem(tocId) {
            root.removeTableOfContentsItem(tocId);
        }

        function onEditPdfTableOfContentsItem(tocId, title) {
            root.prepareTableOfContentsItemEdit(tocId, title);
        }

    }

    Connections {
        target: Translator

        function onTranslationReady(requestId, translatedString, hasTranscription) {
            if (requestId !== root.translationRequestId) {
                return;
            }

            root.translationRequestId = 0;
            root.showTranslation(translatedString, hasTranscription);
        }
    }

    OkularPdfDocument {
        id: document

        onStatusChanged: {
            if (document.status !== OkularPdfDocument.Ready) {
                root.stopSpeech();
                root.speechSelectionText = "";
                root.speechSelectionPosition = ({});
            }
            if (document.status === OkularPdfDocument.Ready) {
                root.loadTableOfContentsDrawer();
                root.loadPersonalPdfAnnotations();
                root.loadPdfAriannaAnchors();
                root.applyInitialLocation();
                Qt.callLater(root.applyInitialSearch);
                root.bookReady(root.displayTitle);
            } else {
                root.loadTableOfContentsDrawer();
            }
        }

        onPageCountChanged: root.applyInitialLocation()
        onTableOfContentsChanged: root.loadTableOfContentsDrawer()


    }

    PdfPasswordHandler {
        id: passwordHandler
        document: document
        // Identify the original PDF, not its session-token URL or downloaded copy.
        source: root.filename ? applicationWindow().fileUrlFromLocalPath(root.filename) : root.url
        bookId: root.bookServerIdentifier()
        window: applicationWindow()
        onPasswordRequested: message => {
            passwordMessage.text = message;
            passwordDialog.open();
        }
        onDismissRequested: {
            passwordDialog.close();
            passwordField.clear();
            rememberPassword.checked = false;
        }
        onNotification: message => applicationWindow().showPassiveNotification(message, "long")
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
        pageSpacing: root.effectivePageSpacing
        coverPageMode: root.coverPageMode
        inverted: root.pdfPageInverted
        recolor: root.pdfPageUsesSystemColors
        foregroundColor: Kirigami.Theme.textColor
        backgroundColor: Kirigami.Theme.backgroundColor
        readerBackgroundImageSource: root.readerBackgroundImageSource()
        searchString: root.activeSearchText
        zoomStep: root.scaleStep
        primaryTableOfContentsPages: root.primaryTableOfContentsPages
        notInvertedRegionSelectionMode: root.notInvertedRegionSelectionMode
        ovalNotInvertedRegionSelectionMode: root.ovalNotInvertedRegionSelectionMode
        forcedInvertedRegionSelectionMode: root.forcedInvertedRegionSelectionMode
        pdfLinkTargetSelectionMode: root.pdfLinkTargetSelectionMode
        allowPdfDocumentEdits: !root.readOnly && root.filename.length > 0
        allowExternalBookReferences: !root.readOnly && root.filename.length > 0 && root.bookServerIdentifier().length > 0
        pdfEditorPageSwitchConfigured: root.pdfEditorPageSwitchConfigured()
        allowPersonalAnnotations: !root.readOnly && root.personalAnnotationBookId().length > 0
        personalAnnotations: root.personalPdfAnnotations
        ariannaAnchors: root.pdfAriannaAnchors
        notInvertedRegionsVersion: root.pdfNotInvertedRegionsVersion
        notInvertedRegionsForPage: page => root.notInvertedRegionsForPage(page)
        forcedInvertedRegionsForPage: page => root.forcedInvertedRegionsForPage(page)

        onCurrentPageChanged: {
            root.speechSelectionText = "";
            root.speechSelectionPosition = ({});
            root.updateReadingPosition();
        }
        onSpeechSelectionChanged: text => {
            root.speechSelectionText = text;
            root.speechSelectionPosition = document.speechSelectionRange();
        }
        onReadSelectionRequested: text => root.readSpeechSelection(text)
        onReadDocumentRequested: position => root.readDocumentFrom(position)
        onSearchRequested: text => {
            root.searchText = text;
            root.executePdfSearch();
        }
        onZoomRequested: factor => root.adjustZoom(factor)
        onPageSpacingChangeRequested: delta => root.changePageSpacing(delta)
        onPersonalAnnotationNavigationRequested: previous => root.navigatePersonalPdfAnnotation(previous)
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
        onPersonalNoteRequested: selection => root.openPersonalPdfNote(selection)
        onCreatePdfLinkRequested: sourceSelection => root.startPdfLinkTargetSelection(sourceSelection)
        onCreateExternalBookReferenceRequested: sourceSelection => root.openExternalBookReferenceDialog(sourceSelection)
        onPdfLinkTargetSelected: targetSelection => root.completePdfLinkTargetSelection(targetSelection)
        onAriannaAnchorActivated: anchorId => root.openPdfAriannaAnchor(anchorId)
        onEditCurrentPageRequested: page => root.editBook(page)
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

    QQC2.Frame {
        anchors {
            top: parent.top
            horizontalCenter: parent.horizontalCenter
            topMargin: Kirigami.Units.largeSpacing
        }
        z: 20
        visible: root.pdfLinkTargetSelectionMode

        contentItem: RowLayout {
            spacing: Kirigami.Units.smallSpacing

            QQC2.Label {
                text: i18nc("@info", "Select the PDF link target")
            }

            QQC2.Button {
                text: i18nc("@action:button", "Cancel")
                icon.name: "dialog-cancel"
                onClicked: root.cancelPdfLinkTargetSelection()
            }
        }
    }

    Shortcut {
        sequence: "Esc"
        enabled: root.pdfLinkTargetSelectionMode
        onActivated: root.cancelPdfLinkTargetSelection()
    }

    ListModel {
        id: externalReferenceBookModel
    }

    ListModel {
        id: externalReferenceTargetLocationModel
    }

    QQC2.Dialog {
        id: pdfReferenceDialog
        objectName: "pdfReferenceDialog"

        property var reference: ({})
        property bool openingTarget: false
        property string error: ""

        modal: true
        parent: QQC2.Overlay.overlay
        title: String(reference.sourceAnchorTitle || i18n("Reference"))
        width: Math.min(Kirigami.Units.gridUnit * 38, parent ? parent.width - Kirigami.Units.gridUnit * 2 : Kirigami.Units.gridUnit * 38)
        x: parent ? Math.round((parent.width - width) / 2) : 0
        y: parent ? Math.round((parent.height - height) / 2) : 0

        contentItem: ColumnLayout {
            QQC2.ScrollView {
                id: referencePreviewScroll

                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(Kirigami.Units.gridUnit * 16, referencePreview.implicitHeight + Kirigami.Units.largeSpacing)
                contentWidth: availableWidth

                QQC2.Label {
                    id: referencePreview

                    width: referencePreviewScroll.availableWidth
                    text: String(pdfReferenceDialog.reference.targetPreviewHtml || i18n("No preview available"))
                    textFormat: Text.AutoText
                    wrapMode: Text.WordWrap
                }
            }

            QQC2.BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                visible: running
                running: pdfReferenceDialog.openingTarget
            }

            QQC2.Label {
                Layout.fillWidth: true
                text: pdfReferenceDialog.error
                visible: text.length > 0
                color: Kirigami.Theme.negativeTextColor
                wrapMode: Text.WordWrap
            }
        }

        footer: RowLayout {
            Item { Layout.fillWidth: true }

            QQC2.Button {
                text: i18n("Close")
                onClicked: pdfReferenceDialog.close()
            }

            QQC2.Button {
                objectName: "editPdfReferenceButton"
                text: i18n("Edit Reference")
                icon.name: "document-edit"
                enabled: !root.readOnly && !pdfReferenceDialog.openingTarget
                onClicked: root.editPdfAriannaAnchor(pdfReferenceDialog.reference.sourceAnchorId)
            }

            QQC2.Button {
                text: i18n("Open Reference")
                icon.name: "go-jump"
                enabled: !pdfReferenceDialog.openingTarget && Boolean(pdfReferenceDialog.reference.targetLocation)
                onClicked: root.followPdfAriannaAnchor(pdfReferenceDialog.reference.sourceAnchorId)
            }
        }
    }

    QQC2.Dialog {
        id: externalBookReferenceDialog
        objectName: "externalBookReferenceDialog"

        modal: true
        parent: QQC2.Overlay.overlay
        title: root.editingExternalBookReference ? i18n("Edit Reference") : i18n("Create External Book Reference")
        width: Math.min(Kirigami.Units.gridUnit * 38, parent ? parent.width - Kirigami.Units.gridUnit * 2 : Kirigami.Units.gridUnit * 38)
        x: parent ? Math.round((parent.width - width) / 2) : 0
        y: parent ? Math.round((parent.height - height) / 2) : 0

        onAboutToHide: {
            ++root.externalReferenceDialogGeneration;
            ++root.externalReferenceLocationGeneration;
            root.externalReferenceBooksLoading = false;
            root.externalReferenceLocationsLoading = false;
            root.pendingExternalBookReferenceSource = null;
            root.editingExternalBookReference = null;
        }

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            QQC2.Label {
                text: i18n("Source Anchor Title:")
            }

            QQC2.TextField {
                id: externalReferenceSourceAnchorTitle
                objectName: "externalReferenceSourceAnchorTitle"

                Layout.fillWidth: true
            }

            QQC2.Label {
                text: i18n("Target Book:")
            }

            QQC2.ComboBox {
                id: externalReferenceBookCombo
                objectName: "externalReferenceBookCombo"

                Layout.fillWidth: true
                model: externalReferenceBookModel
                textRole: "title"
                onActivated: root.loadExternalReferenceTargetLocations()

                delegate: QQC2.ItemDelegate {
                    required property int index
                    required property string title
                    required property bool selectable
                    required property bool category

                    width: externalReferenceBookCombo.width
                    text: title
                    enabled: selectable
                    highlighted: externalReferenceBookCombo.highlightedIndex === index && selectable
                    font.bold: category
                    leftPadding: category ? Kirigami.Units.largeSpacing : Kirigami.Units.gridUnit * 2
                    onClicked: {
                        if (!selectable) {
                            return;
                        }
                        externalReferenceBookCombo.currentIndex = index;
                        externalReferenceBookCombo.popup.close();
                        root.loadExternalReferenceTargetLocations();
                    }
                }

            }

            QQC2.Label {
                text: i18n("Target Location:")
            }

            QQC2.ComboBox {
                id: externalReferenceTargetLocationCombo
                objectName: "externalReferenceTargetLocationCombo"

                Layout.fillWidth: true
                model: externalReferenceTargetLocationModel
                textRole: "title"
                onActivated: root.updateExternalReferencePreview()
            }

            QQC2.Label {
                text: i18n("Explicit Target Location:")
            }

            QQC2.TextField {
                id: externalReferenceTargetLocationInput
                objectName: "externalReferenceTargetLocationInput"

                property bool edited: false

                Layout.fillWidth: true
                selectByMouse: true
                onTextEdited: {
                    edited = true;
                    const location = root.selectedExternalReferenceTargetLocation();
                    if (location && text !== location.targetLocation) {
                        externalReferenceTargetLocationCombo.currentIndex = 0;
                    }
                }
            }

            QQC2.Label {
                text: i18n("Preview Text:")
            }

            QQC2.TextArea {
                id: externalReferencePreviewHtml
                objectName: "externalReferencePreviewHtml"
                property bool edited: false

                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 8
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                onTextChanged: edited = true
            }

            QQC2.BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                running: root.externalReferenceBooksLoading || root.externalReferenceLocationsLoading
                visible: running
            }

            QQC2.Label {
                Layout.fillWidth: true
                visible: root.externalReferenceError.length > 0
                text: root.externalReferenceError
                color: Kirigami.Theme.negativeTextColor
                wrapMode: Text.WordWrap
            }
        }

        footer: RowLayout {
            spacing: Kirigami.Units.smallSpacing

            Item {
                Layout.fillWidth: true
            }

            QQC2.Button {
                text: i18n("Cancel")
                onClicked: externalBookReferenceDialog.close()
            }

            QQC2.Button {
                objectName: "saveExternalBookReferenceButton"
                text: root.editingExternalBookReference ? i18n("save change of reference") : i18n("Create External Book Reference")
                icon.name: root.editingExternalBookReference ? "document-save" : "insert-link"
                enabled: !root.readOnly && !root.externalReferenceBooksLoading
                    && root.selectedExternalReferenceBook() !== null && externalReferenceTargetLocationInput.text.trim().length > 0
                onClicked: root.saveExternalBookReference()
            }
        }
    }

    Kirigami.Dialog {
        id: pdfReferencesDialog
        objectName: "pdfReferencesDialog"

        parent: root
        modal: true
        title: i18n("References")
        width: Math.min(Kirigami.Units.gridUnit * 40, applicationWindow().width - Kirigami.Units.gridUnit * 4)
        height: Math.min(Kirigami.Units.gridUnit * 35, applicationWindow().height - Kirigami.Units.gridUnit * 4)

        contentItem: Item {
            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.largeSpacing * 2
                visible: root.pdfAriannaAnchors.length === 0
                text: i18n("No references in this book")
                icon.name: "emblem-symbolic-link"
            }

            ListView {
                id: pdfReferencesList
                anchors.fill: parent
                anchors.margins: Kirigami.Units.largeSpacing
                model: root.pdfAriannaAnchors
                spacing: Kirigami.Units.smallSpacing
                clip: true
                QQC2.ScrollBar.vertical: QQC2.ScrollBar {}

                delegate: Delegates.RoundedItemDelegate {
                    id: pdfReferenceDelegate
                    objectName: "pdfReferenceDelegate"
                    required property var modelData

                    width: pdfReferencesList.width
                    QQC2.ToolTip.text: modelData.sourceAnchorTitle || modelData.anchorId
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    onClicked: root.goToPdfAriannaAnchor(modelData)

                    contentItem: ColumnLayout {
                        spacing: Kirigami.Units.smallSpacing

                        QQC2.Label {
                            Layout.fillWidth: true
                            text: pdfReferenceDelegate.modelData.sourceAnchorTitle || i18n("Reference")
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                            font.weight: Font.Medium
                        }

                        QQC2.Label {
                            text: i18n("Page %1", Number(pdfReferenceDelegate.modelData.page) + 1)
                            font: Kirigami.Theme.smallFont
                            color: Kirigami.Theme.disabledTextColor
                        }
                    }
                }
            }
        }
    }

    Kirigami.Dialog {
        id: personalPdfNotesDialog
        objectName: "personalPdfNotesDialog"

        parent: root
        modal: true
        title: i18n("Remarks")
        width: Math.min(Kirigami.Units.gridUnit * 40, applicationWindow().width - Kirigami.Units.gridUnit * 4)
        height: Math.min(Kirigami.Units.gridUnit * 35, applicationWindow().height - Kirigami.Units.gridUnit * 4)

        contentItem: Item {
            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.largeSpacing * 2
                visible: root.personalPdfAnnotations.length === 0
                text: i18n("No remarks in this book")
                icon.name: "comment"
            }

            ListView {
                id: personalPdfNotesList
                anchors.fill: parent
                anchors.margins: Kirigami.Units.largeSpacing
                model: root.personalPdfAnnotations
                spacing: Kirigami.Units.smallSpacing
                clip: true
                QQC2.ScrollBar.vertical: QQC2.ScrollBar {}

                delegate: Delegates.RoundedItemDelegate {
                    id: personalPdfNoteDelegate
                    objectName: "personalPdfNoteDelegate"
                    required property var modelData

                    width: personalPdfNotesList.width
                    onClicked: root.goToPersonalPdfAnnotation(modelData)

                    contentItem: ColumnLayout {
                        spacing: Kirigami.Units.smallSpacing

                        RowLayout {
                            Layout.fillWidth: true

                            Rectangle {
                                Layout.preferredWidth: Kirigami.Units.gridUnit
                                Layout.preferredHeight: Kirigami.Units.gridUnit * 1.5
                                color: personalPdfNoteDelegate.modelData.color || "yellow"
                                radius: Kirigami.Units.smallSpacing
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0

                                QQC2.Label {
                                    Layout.fillWidth: true
                                    text: personalPdfNoteDelegate.modelData.text || i18n("Highlighted text")
                                    textFormat: Text.PlainText
                                    wrapMode: Text.WordWrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                    font.weight: Font.Medium
                                }

                                QQC2.Label {
                                    text: i18n("Page %1", Number(personalPdfNoteDelegate.modelData.location.page) + 1)
                                    font: Kirigami.Theme.smallFont
                                    color: Kirigami.Theme.disabledTextColor
                                }
                            }

                            QQC2.ToolButton {
                                objectName: "editPersonalPdfNoteButton"
                                display: QQC2.AbstractButton.IconOnly
                                icon.name: "document-edit"
                                text: i18n("Edit Annotation")
                                enabled: !root.readOnly
                                QQC2.ToolTip.text: text
                                QQC2.ToolTip.visible: hovered
                                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                                onClicked: root.editPersonalPdfAnnotation(personalPdfNoteDelegate.modelData.annotationId)
                            }

                            QQC2.ToolButton {
                                objectName: "deletePersonalPdfNoteButton"
                                display: QQC2.AbstractButton.IconOnly
                                icon.name: "edit-delete"
                                text: i18n("Delete Annotation")
                                enabled: !root.readOnly
                                QQC2.ToolTip.text: text
                                QQC2.ToolTip.visible: hovered
                                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                                onClicked: Qt.callLater(root.removePersonalPdfAnnotation, personalPdfNoteDelegate.modelData.annotationId)
                            }
                        }

                        QQC2.Label {
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: personalPdfNoteDelegate.modelData.note || ""
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                            font: Kirigami.Theme.smallFont
                        }
                    }
                }
            }
        }
    }

    QQC2.Dialog {
        id: personalPdfNoteDialog
        objectName: "personalPdfNoteDialog"

        modal: true
        parent: QQC2.Overlay.overlay
        title: root.editingPersonalPdfAnnotation ? i18n("Edit Annotation") : i18nc("@title:dialog", "Add Note")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        width: Math.min(Kirigami.Units.gridUnit * 30, parent ? parent.width - Kirigami.Units.gridUnit * 2 : Kirigami.Units.gridUnit * 30)
        x: parent ? Math.round((parent.width - width) / 2) : 0
        y: parent ? Math.round((parent.height - height) / 2) : 0

        onAccepted: root.savePersonalPdfNote()
        onRejected: {
            root.pendingPersonalPdfNote = null;
            root.editingPersonalPdfAnnotation = null;
        }
        onOpened: personalPdfNoteText.forceActiveFocus()

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            QQC2.Label {
                id: personalPdfNoteExcerpt

                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                maximumLineCount: 4
                elide: Text.ElideRight
                textFormat: Text.PlainText
            }

            QQC2.TextArea {
                id: personalPdfNoteText
                objectName: "personalPdfNoteText"

                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 8
                wrapMode: Text.Wrap
                placeholderText: i18n("Add Note…")
            }
            AnnotationColorPalette {
                id: personalPdfNoteColors
                objectName: "personalPdfNoteColors"
                enabled: !root.readOnly
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
                    id: translatedText
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

    ColumnLayout {
        anchors.centerIn: parent
        z: 20
        visible: document.needsPassword && !passwordDialog.visible

        QQC2.BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            running: passwordHandler.busy
            visible: running
        }
        QQC2.Label {
            text: passwordHandler.busy ? i18nc("@info:progress", "Looking up the PDF password in KWallet…") : i18n("Password Required")
        }
        QQC2.Button {
            Layout.alignment: Qt.AlignHCenter
            text: i18nc("@action:button", "Enter Password")
            onClicked: passwordHandler.enterManually()
        }
    }

    QQC2.Dialog {
        id: passwordDialog
        objectName: "pdfPasswordDialog"

        modal: true
        parent: QQC2.Overlay.overlay
        title: i18n("Password Required")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel

        onOpened: passwordField.forceActiveFocus()
        onAccepted: {
            const password = passwordField.text;
            passwordField.clear();
            passwordHandler.submit(password, rememberPassword.checked);
        }
        onRejected: {
            passwordField.clear();
            passwordHandler.cancel();
        }

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            QQC2.TextField {
                id: passwordField

                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: i18n("Password")
                onAccepted: passwordDialog.accept()
            }

            QQC2.Label {
                id: passwordMessage
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                visible: text.length > 0
                wrapMode: Text.Wrap
            }

            QQC2.CheckBox {
                id: rememberPassword
                text: i18nc("@option:check", "Save password in KWallet")
            }
        }
    }

    QQC2.Dialog {
        id: pdfLinkPermissionDialog

        modal: true
        parent: QQC2.Overlay.overlay
        title: i18nc("@title:dialog", "PDF Link Cannot Be Created")
        standardButtons: QQC2.Dialog.Close

        contentItem: QQC2.Label {
            width: Kirigami.Units.gridUnit * 28
            text: i18n("The encrypted PDF cannot be modified without owner permissions.")
            wrapMode: Text.Wrap
        }
    }

    QQC2.Dialog {
        id: manualTableOfContentsDialog

        modal: true
        parent: QQC2.Overlay.overlay
        title: i18nc("@title:dialog", "Add Page to Table of Contents")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        onAccepted: root.addManualTableOfContentsEntry()
        onRejected: {
            manualTableOfContentsTitle.clear();
            root.manualTableOfContentsTargets = [];
        }

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

            QQC2.Label {
                text: i18nc("@label", "Insert relative to:")
            }

            QQC2.ComboBox {
                id: manualTableOfContentsTarget

                Layout.fillWidth: true
                model: root.manualTableOfContentsTargets
                textRole: "title"
                delegate: QQC2.ItemDelegate {
                    required property var modelData

                    width: manualTableOfContentsTarget.width
                    text: modelData.title
                    topPadding: modelData.isAppendTarget ? Kirigami.Units.largeSpacing : Kirigami.Units.smallSpacing
                    highlighted: manualTableOfContentsTarget.highlightedIndex === index
                    onClicked: {
                        manualTableOfContentsTarget.currentIndex = index;
                        manualTableOfContentsTarget.popup.close();
                    }

                    QQC2.MenuSeparator {
                        anchors {
                            top: parent.top
                            left: parent.left
                            right: parent.right
                        }
                        visible: modelData.isAppendTarget
                    }
                }
            }

            QQC2.Label {
                text: i18nc("@label", "Position:")
            }

            QQC2.ComboBox {
                id: manualTableOfContentsPlacement

                Layout.fillWidth: true
                enabled: manualTableOfContentsTarget.currentIndex >= 0
                    && root.manualTableOfContentsTargets[manualTableOfContentsTarget.currentIndex]
                    && !root.manualTableOfContentsTargets[manualTableOfContentsTarget.currentIndex].isAppendTarget
                textRole: "text"
                valueRole: "value"
                model: [{
                        text: i18nc("@item:inlistbox", "Before selected item"),
                        value: "before"
                    }, {
                        text: i18nc("@item:inlistbox", "After selected item"),
                        value: "after"
                    }, {
                        text: i18nc("@item:inlistbox", "As subitem of selected item"),
                        value: "child"
                    }]
            }
        }
    }

    QQC2.Dialog {
        id: editTableOfContentsItemDialog

        property string tocId: ""

        modal: true
        parent: QQC2.Overlay.overlay
        title: i18nc("@title:dialog", "Edit Table of Contents Item")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        onAccepted: root.editTableOfContentsItem()
        onRejected: {
            tocId = "";
            editTableOfContentsItemTitle.clear();
        }

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            QQC2.Label {
                text: i18nc("@label", "Title:")
            }

            QQC2.TextField {
                id: editTableOfContentsItemTitle

                Layout.fillWidth: true
                onAccepted: editTableOfContentsItemDialog.accept()
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
            if (root.headerSearchField) {
                root.headerSearchField.forceActiveFocus();
                root.headerSearchField.selectAll();
            }
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
                visible: root.activeSearchText.length > 0
                spacing: Kirigami.Units.smallSpacing
                clip: true

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
                    visible: root.activeSearchText.length > 0
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
