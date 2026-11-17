// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import "reference-utils.mjs" as ReferenceUtils

Kirigami.Dialog {
    id: root
    objectName: "crossReferenceEditor"
    parent: QQC2.Overlay.overlay
    anchors.centerIn: parent
    property string serverUrl: ""
    property string sessionToken: ""
    property string sourceBookId: ""
    property string sourceCfi: ""
    property string sourceText: ""
    property var options: ({})
    property var books: []
    property var bookRows: []
    property var targetRows: []
    property string targetBookId: ""
    property string targetLocation: ""
    property string previewHtml: ""
    property string sourceAnchorTitle: ""
    property bool autoTitle: true
    property bool locationEdited: false
    property bool previewEdited: false
    property string bookState: "idle"
    property string targetState: "idle"
    property string errorMessage: ""
    property string targetError: ""
    property var targetAnchorRepair: ({})
    onTargetBookIdChanged: targetAnchorRepair = ({})
    onTargetLocationChanged: targetAnchorRepair = ({})
    readonly property bool editMode: !!options.editMode
    readonly property bool saving: saveRequest.running
    readonly property bool canSave: bookState === "ready" && targetState === "ready"
        && !!targetBookId && !!targetLocation.trim() && !saving
    signal saved(var response, bool edited)
    signal manualAnchorRequired(var response)

    title: editMode ? i18n("Edit Reference") : i18n("Create Reference")
    modal: true
    standardButtons: QQC2.Dialog.NoButton
    padding: Kirigami.Units.largeSpacing
    closePolicy: saving ? QQC2.Popup.NoAutoClose : QQC2.Popup.CloseOnEscape
    showCloseButton: !saving
    width: Math.min(Kirigami.Units.gridUnit * 46, parent ? parent.width - 24 : 850)
    height: Math.min(Kirigami.Units.gridUnit * 46, parent ? parent.height - 24 : 850)

    function begin(bookId, cfi, text, preferred) {
        if (saving) return;
        cancelReads();
        sourceBookId = bookId;
        sourceCfi = cfi;
        sourceText = text;
        options = JSON.parse(JSON.stringify(preferred || {}));
        targetBookId = options.targetBookId || sourceBookId;
        targetLocation = options.targetHrefLocation || options.targetLocation || "";
        previewHtml = options.targetPreviewHtml ?? "";
        sourceAnchorTitle = options.sourceAnchorTitle || "";
        autoTitle = !sourceAnchorTitle;
        locationEdited = previewEdited = false;
        bookSearch.query = targetSearch.query = "";
        books = bookRows = targetRows = [];
        targetState = "idle";
        errorMessage = targetError = "";
        targetAnchorRepair = options.targetAnchorRepair || ({});
        open();
        loadBooks();
    }

    function cancelReads() {
        booksRequest.cancel();
        targetsRequest.cancel();
    }
    onClosed: {
        cancelReads();
        books = bookRows = targetRows = [];
        targetAnchorRepair = ({});
    }

    function loadBooks() {
        errorMessage = "";
        bookState = "loading";
        booksRequest.send("GET", "/books");
    }
    function groupBooks() {
        if (!books.some(book => book.id === sourceBookId))
            books.push({id: sourceBookId, title: sourceBookId});
        if (targetBookId && !books.some(book => book.id === targetBookId))
            books.push({id: targetBookId, title: targetBookId});
        const own = books.find(book => book.id === sourceBookId);
        const authors = ReferenceUtils.authorsForBook(own).join(", ");
        const topics = ReferenceUtils.mainSubjectsForBook(own).join(", ");
        bookRows = ReferenceUtils.groupedReferenceBooks(books, sourceBookId, [i18n("This book"),
            i18n("Books by %1", authors), i18n("Main topic: %1", topics), i18n("Other books")]);
        bookState = "ready";
        suggestTitle();
        loadTargets();
    }
    function chooseBook(row) {
        if (saving || row.value === targetBookId) return;
        targetBookId = row.value;
        targetLocation = previewHtml = "";
        locationEdited = previewEdited = false;
        options = Object.assign({}, options, {targetLocation: "", targetHrefLocation: "", targetCfiLocation: "", targetPreviewHtml: ""});
        targetSearch.query = "";
        loadTargets();
        suggestTitle();
    }
    function loadTargets() {
        targetRows = [];
        targetError = "";
        targetState = targetBookId ? "loading" : "idle";
        targetsRequest.send("GET", "/" + encodeURIComponent(targetBookId) + "/targetLocations", null, 60000);
    }
    function chooseTarget(row, preservePreview = false) {
        targetLocation = row.value;
        locationEdited = false;
        if (!preservePreview) { previewHtml = row.data.previewHtml || ""; previewEdited = false; }
        suggestTitle(row.data);
    }
    function suggestTitle(target = null) {
        if (!autoTitle) return;
        if (!target) target = targetRows.find(row => row.value === targetLocation)?.data;
        sourceAnchorTitle = ReferenceUtils.suggestedSourceAnchorTitle(books.find(book => book.id === targetBookId), target, sourceText)
            || ReferenceUtils.defaultSourceAnchorTitle;
    }
    function save() {
        if (!canSave) return;
        errorMessage = "";
        saveRequest.send("POST", "/" + encodeURIComponent(sourceBookId) + "/ref", {
            cfi: sourceCfi, text: sourceText, sourceAnchorId: options.sourceAnchorId || "",
            sourceAnchorTitle: sourceAnchorTitle.trim(), targetBookId, targetLocation: targetLocation.trim(),
            targetPreviewHtml: previewHtml, targetPreviewHtmlEdited: previewEdited
        }, 60000);
    }
    function completeSave(response) {
        const modifiedBookIds = Array.from(response.modifiedBookIds || []);
        if (!modifiedBookIds.includes(sourceBookId)) modifiedBookIds.push(sourceBookId);
        const result = Object.assign({}, response, {modifiedBookIds});
        const edited = editMode;
        close();
        saved(result, edited);
    }

    BookServerRequest {
        id: booksRequest
        baseUrl: root.serverUrl; sessionToken: root.sessionToken
        onFinished: (response, error) => {
            if (error) { root.bookState = "error"; root.errorMessage = error.message; return; }
            root.books = Array.isArray(response) ? response : response.books || [];
            root.groupBooks();
        }
    }
    BookServerRequest {
        id: targetsRequest
        objectName: "crossReferenceTargetsRequest"
        baseUrl: root.serverUrl; sessionToken: root.sessionToken
        onFinished: (response, error) => {
            if (error) {
                root.targetState = "error";
                root.targetError = error.status === 401 ? i18n("The BookServer session has expired. Restart Arianna.")
                    : error.status === 404 ? i18n("The target book or its file is no longer available.") : error.message;
                return;
            }
            const locations = Array.isArray(response) ? response : response.targetLocations || [];
            root.targetRows = locations.filter(location => location.id && (location.hrefLocation || location.location || location.readerLocation))
                .map(location => {
                    const value = location.hrefLocation || location.location || location.readerLocation;
                    const label = ReferenceUtils.optionLabelForTargetLocation(location);
                    const tooltip = [Array.from(location.tocPath || []).join(" / "), value,
                        ReferenceUtils.textPreviewForTargetLocation(location)].filter(Boolean).join("\n");
                    return {value, label, tooltip, data: location, searchKey: ReferenceUtils.referenceSearchKey(label + " " + tooltip)};
                });
            root.targetState = "ready";
            if (!root.locationEdited) {
                const aliases = [root.targetLocation, root.options.targetCfiLocation].filter(Boolean);
                const selected = root.targetRows.find(row => aliases.includes(row.value) || aliases.includes(row.data.cfiLocation));
                if (selected) root.chooseTarget(selected, true);
            }
            root.suggestTitle();
        }
    }
    BookServerRequest {
        id: saveRequest
        baseUrl: root.serverUrl; sessionToken: root.sessionToken
        onFinished: (response, error) => {
            if (!error) { root.completeSave(response); return; }
            if (error.responseJson?.error === "manualAnchorRequired") {
                const detail = Object.assign({cfi: root.sourceCfi, text: root.sourceText}, error.responseJson);
                if (detail.referenceStored) root.completeSave(detail);
                root.manualAnchorRequired(detail);
            }
            root.errorMessage = error.message;
            root.targetAnchorRepair = error.responseJson?.targetAnchorRepair || ({});
        }
    }

    contentItem: ColumnLayout {
        QQC2.ScrollView {
            id: scroll
            Layout.fillWidth: true; Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                width: scroll.availableWidth
                spacing: Kirigami.Units.smallSpacing
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: !!root.errorMessage
                    text: root.errorMessage
                    type: Kirigami.MessageType.Error
                }
                TargetAnchorRepair {
                    Layout.fillWidth: true
                    repair: root.targetAnchorRepair
                    enabled: !root.saving
                }
                QQC2.Label { text: i18n("Source anchor title:") }
                QQC2.TextField {
                    objectName: "crossReferenceSourceTitle"
                    Layout.fillWidth: true
                    text: root.sourceAnchorTitle
                    selectByMouse: true
                    onTextEdited: { root.autoTitle = false; root.sourceAnchorTitle = text; }
                }
                Kirigami.Heading { text: i18n("Target book"); level: 3 }
                RowLayout {
                    visible: root.bookState === "loading"
                    QQC2.BusyIndicator { running: parent.visible; Layout.preferredWidth: Kirigami.Units.gridUnit * 2; Layout.preferredHeight: width }
                    QQC2.Label { text: i18n("Loading books…") }
                }
                QQC2.Button {
                    visible: root.bookState === "error"
                    text: i18n("Reload Books")
                    onClicked: root.loadBooks()
                }
                ReferenceSearchList {
                    id: bookSearch
                    objectName: "crossReferenceBooks"
                    Layout.fillWidth: true
                    rows: root.bookRows
                    enabled: root.bookState === "ready" && !root.saving
                    placeholderText: i18n("Search books by title or author…")
                    selectedValue: root.targetBookId
                    selectedLabel: root.bookRows.find(row => row.value === root.targetBookId)?.label || root.targetBookId
                    onSelected: row => root.chooseBook(row)
                }
                Kirigami.Heading { text: i18n("Target location"); level: 3 }
                RowLayout {
                    visible: root.targetState === "loading"
                    QQC2.BusyIndicator { running: parent.visible; Layout.preferredWidth: Kirigami.Units.gridUnit * 2; Layout.preferredHeight: width }
                    QQC2.Label { text: i18n("Loading target locations…") }
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: !!root.targetError
                    text: root.targetError
                    type: Kirigami.MessageType.Error
                }
                QQC2.Button {
                    objectName: "crossReferenceRetryTargets"
                    visible: root.targetState === "error"
                    text: i18n("Reload Target Locations")
                    onClicked: root.loadTargets()
                }
                ReferenceTargetTree {
                    id: targetSearch
                    objectName: "crossReferenceTargets"
                    Layout.fillWidth: true
                    rows: root.targetRows
                    enabled: root.targetState === "ready" && !root.saving
                    placeholderText: i18n("Search targets by title, text or anchor…")
                    selectedValue: root.targetLocation
                    selectedLabel: root.targetLocation
                    onSelected: row => root.chooseTarget(row)
                }
                QQC2.Label { text: i18n("Explicit target location:") }
                QQC2.TextField {
                    objectName: "crossReferenceTargetLocation"
                    Layout.fillWidth: true
                    text: root.targetLocation
                    enabled: !root.saving
                    selectByMouse: true
                    onTextEdited: { root.locationEdited = true; root.targetLocation = text; }
                }
                QQC2.Label { text: i18n("Preview text (HTML):") }
                QQC2.ScrollView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 6
                    QQC2.TextArea {
                        objectName: "crossReferencePreviewText"
                        text: root.previewHtml
                        enabled: !root.saving
                        textFormat: TextEdit.PlainText
                        wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        onTextChanged: if (root.previewHtml !== text) { root.previewHtml = text; root.previewEdited = true; }
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            QQC2.BusyIndicator { visible: root.saving; running: visible; Layout.preferredWidth: Kirigami.Units.gridUnit * 2; Layout.preferredHeight: width }
            Item { Layout.fillWidth: true }
            QQC2.Button { text: i18n("Cancel"); enabled: !root.saving; onClicked: root.close() }
            QQC2.Button {
                objectName: "crossReferenceSave"
                text: root.editMode ? i18n("Save Changes") : i18n("Create Reference")
                icon.name: "document-save"
                enabled: root.canSave
                onClicked: root.save()
            }
        }
    }
}
