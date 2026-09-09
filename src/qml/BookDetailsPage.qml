// SPDX-FileCopyrightText: Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import org.kde.arianna as Arianna

FormCard.FormCardPage {
    id: root

    property var metadata
    property BookListModel bookListModel
    property bool readOnly: false
    property var importToLibrary: null
    property bool importRunning: false
    property string importStatus: ""
    property string checkStatus: ""
    property bool checkFailed: false
    property bool checkRunning: false
    property string epubCheckStatus: ""
    property string epubCheckOutput: ""
    property bool epubCheckFailed: false
    property bool epubCheckRunning: false
    property string pdfCheckStatus: ""
    property var pdfCheckReport: ({})
    property string pdfCheckRawOutput: ""
    property bool pdfCheckFailed: false
    property bool pdfCheckRunning: false
    property string watermarkStatus: ""
    property bool watermarkRunning: false
    property string pdfVersionStatus: ""
    property bool pdfVersionUpdating: false
    property string pendingWatermarkReplacement: ""
    property int pendingWatermarkOccurrences: 0
    property var topicPathCandidates: []
    property var topicPathSuggestions: []
    property bool applyingTopicSuggestion: false
    property string detectedBookVersion: ""
    readonly property bool wideLayout: width >= Kirigami.Units.gridUnit * 36
    readonly property string epubTypeIconSource: "qrc:/qt/qml/org/kde/arianna/qml/icons/epub_icon.png"
    readonly property string pdfTypeIconSource: "qrc:/qt/qml/org/kde/arianna/qml/icons/pdf_icon.png"

    implicitWidth: Kirigami.Units.gridUnit * 44

    function normalizedIdentifier(identifier) {
        let value = identifier ? String(identifier).trim() : "";
        if (value.toLowerCase().startsWith("urn:uuid:")) {
            value = value.slice(9);
        }
        return value.toLowerCase();
    }

    function otherIdentifiers() {
        const uniqueIdentifier = normalizedIdentifier(root.metadata.uniqueIdentifier);
        return (root.metadata.identifier || "").split(",").map(identifier => identifier.trim()).filter(identifier => {
            return identifier.length > 0 && (!uniqueIdentifier || normalizedIdentifier(identifier) !== uniqueIdentifier);
        }).join(", ");
    }

    function listValues(value) {
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
        for (const rawItem of items) {
            const item = String(rawItem).trim();
            if (item.length === 0) {
                continue;
            }

            result.push(item);
        }

        return result;
    }

    function escapedHtml(value) {
        return String(value || "")
            .replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;");
    }

    function epubCheckOutputMarkup() {
        const errorColor = Kirigami.Theme.negativeTextColor.toString();
        return root.escapedHtml(root.epubCheckOutput)
            .replace(/\bERROR\b/g, `<span style="color:${errorColor}; font-weight:700;">ERROR</span>`)
            .replace(/\n/g, "<br>");
    }

    function normalizedList(value) {
        const result = [];
        const seen = {};
        const items = root.listValues(value);
        for (const item of items) {
            const key = item.toLowerCase();
            if (seen[key]) {
                continue;
            }

            seen[key] = true;
            result.push(item);
        }
        return result;
    }

    function normalizedTopicPath(value) {
        const segments = String(value).split("/").map(segment => segment.trim()).filter(segment => segment.length > 0);
        return segments.join("/");
    }

    function normalizedTopicList(value) {
        const result = [];
        const seen = {};
        const items = root.listValues(value);
        for (const item of items) {
            const path = root.normalizedTopicPath(item);
            if (path.length === 0) {
                continue;
            }

            const key = path.toLowerCase();
            if (seen[key]) {
                continue;
            }

            seen[key] = true;
            result.push(path);
        }
        return result;
    }

    function topicTreeList(value) {
        const result = [];
        const seen = {};
        const topics = root.normalizedTopicList(value);
        for (const topic of topics) {
            const segments = topic.split("/");
            const path = [];
            for (const segment of segments) {
                path.push(segment);
                const topicPath = path.join("/");
                const key = topicPath.toLowerCase();
                if (seen[key]) {
                    continue;
                }

                seen[key] = true;
                result.push(topicPath);
            }
        }
        return result;
    }

    function leafTopicList(value) {
        const topics = root.topicTreeList(value);
        const leaves = [];
        for (const topic of topics) {
            const key = topic.toLowerCase();
            let hasChild = false;
            for (const candidate of topics) {
                if (candidate.toLowerCase().startsWith(key + "/")) {
                    hasChild = true;
                    break;
                }
            }
            if (!hasChild) {
                leaves.push(topic);
            }
        }
        return leaves;
    }

    function topicsText() {
        if (!root.metadata) {
            return "";
        }

        return root.leafTopicList(root.metadata.subjects || root.metadata.genres).join(", ");
    }

    function authorText() {
        return root.authorTextForMetadata(root.metadata);
    }

    function authorTextForMetadata(metadata) {
        if (!metadata) {
            return "";
        }

        return root.normalizedList(metadata.author).join(", ");
    }

    function canEditAuthor() {
        return !root.readOnly && root.isEpubBook();
    }

    function topicTextForMetadata(metadata) {
        if (!metadata) {
            return "";
        }

        return root.leafTopicList(metadata.subjects || metadata.genres).join(", ");
    }

    function updateMetadata(metadata) {
        if (!metadata || !metadata.filename) {
            return;
        }

        root.metadata = metadata;
        root.detectedBookVersion = root.detectBookVersion(metadata);
        authorEditField.text = root.authorText();
        topicTreeField.text = root.topicsText();
        root.updateTopicPathSuggestions();
    }

    function catalogModel() {
        if (root.bookListModel) {
            return root.bookListModel;
        }

        const window = applicationWindow();
        if (window && window.bookListModel) {
            return window.bookListModel;
        }

        return null;
    }

    function currentMetadataFilename() {
        return root.metadata && root.metadata.filename ? root.metadata.filename : "";
    }

    function isPdfBook() {
        return root.currentMetadataFilename().toLocaleLowerCase().endsWith(".pdf");
    }

    function isEpubBook() {
        return root.currentMetadataFilename().toLocaleLowerCase().endsWith(".epub");
    }

    function bookTypeIconSource() {
        if (root.isPdfBook()) {
            return root.pdfTypeIconSource;
        }
        if (root.isEpubBook()) {
            return root.epubTypeIconSource;
        }
        return "";
    }

    function maintenanceTitle() {
        return root.isPdfBook() ? i18n("PDF Maintenance") : i18n("EPUB Maintenance");
    }

    function checkingMessage() {
        return root.isPdfBook() ? i18n("Checking PDF…") : i18n("Checking EPUB…");
    }

    function checkFailedMessage() {
        return root.isPdfBook() ? i18n("PDF check failed.") : i18n("EPUB check failed.");
    }

    function epubCheckCommand() {
        return (Config.epubCheckCommand || "").trim();
    }

    function pdfCheckCommand() {
        return (Config.pdfCheckCommand || "").trim();
    }

    function pdfCheckSummary(report) {
        return i18n("The PDF check reported %1 and %2.",
            i18np("%1 deviation", "%1 deviations", Number(report.deviations) || 0),
            i18np("%1 failed rule", "%1 failed rules", Number(report.failedRules) || 0));
    }

    function bookVersionLabel() {
        return root.isPdfBook() ? i18n("PDF version:") : i18n("EPUB version:");
    }

    function detectBookVersion(metadata) {
        if (!metadata || !metadata.filename) {
            return "";
        }

        const storedVersion = metadata.bookVersion || "";
        if (storedVersion.length > 0) {
            return storedVersion;
        }

        const model = root.catalogModel();
        if (model && typeof model.bookVersionFromFile === "function") {
            return model.bookVersionFromFile(metadata.filename);
        }

        return "";
    }

    function saveTopics() {
        if (root.readOnly || !root.metadata || !root.metadata.filename) {
            return;
        }

        const filename = root.metadata.filename;
        const topics = root.leafTopicList(topicTreeField.text).join(", ");
        if (topics === root.topicsText()) {
            topicTreeField.text = topics;
            return;
        }

        const model = root.catalogModel();
        if (!model) {
            console.warn("Unable to save topics: book list model is not available");
            return;
        }

        model.setBookData(filename, "subjects", topics);

        const updatedMetadata = model.bookEntryFromFile(filename);
        if (updatedMetadata && updatedMetadata.filename && root.topicTextForMetadata(updatedMetadata) === topics) {
            root.updateMetadata(updatedMetadata);
        } else {
            topicTreeField.text = topics;
        }
    }

    function saveAuthor() {
        if (!root.canEditAuthor() || !root.metadata || !root.metadata.filename) {
            return;
        }

        const filename = root.metadata.filename;
        const author = root.normalizedList(authorEditField.text).join(", ");
        if (author === root.authorText()) {
            authorEditField.text = author;
            return;
        }

        const model = root.catalogModel();
        if (!model) {
            console.warn("Unable to save author: book list model is not available");
            return;
        }

        model.setBookData(filename, "author", author);

        const updatedMetadata = model.bookEntryFromFile(filename);
        if (updatedMetadata && updatedMetadata.filename && root.authorTextForMetadata(updatedMetadata) === author) {
            root.updateMetadata(updatedMetadata);
        } else {
            authorEditField.text = author;
        }
    }

    function refreshTopicPathCandidates() {
        const model = root.catalogModel();
        if (!model) {
            root.topicPathCandidates = [];
            root.topicPathSuggestions = [];
            return;
        }

        if (!model.subjectCategoryModelPopulated) {
            model.populateSubjectCategoryModel();
        }

        const subjectModel = model.subjectCategoryModel;
        if (!subjectModel || typeof subjectModel.flatCategoryEntries !== "function") {
            root.topicPathCandidates = [];
            root.topicPathSuggestions = [];
            return;
        }

        const entries = subjectModel.flatCategoryEntries();
        const candidates = [];
        const seen = {};
        for (const entry of entries) {
            const path = String(entry.title || "").trim();
            if (path.length === 0) {
                continue;
            }

            const key = path.toLocaleLowerCase();
            if (seen[key]) {
                continue;
            }

            seen[key] = true;
            candidates.push({
                path: path,
                display: String(entry.localizedPathTitle || path)
            });
        }

        root.topicPathCandidates = candidates;
        root.updateTopicPathSuggestions();
    }

    function topicTokenRange() {
        const text = topicTreeField.text || "";
        const cursor = Math.max(0, Math.min(topicTreeField.cursorPosition, text.length));
        const commaStart = text.lastIndexOf(",", cursor - 1);
        const semicolonStart = text.lastIndexOf(";", cursor - 1);
        const separatorStart = Math.max(commaStart, semicolonStart);
        let start = separatorStart < 0 ? 0 : separatorStart + 1;
        while (start < cursor && /\s/.test(text.charAt(start))) {
            ++start;
        }

        const commaEnd = text.indexOf(",", cursor);
        const semicolonEnd = text.indexOf(";", cursor);
        let end = text.length;
        if (commaEnd >= 0 && semicolonEnd >= 0) {
            end = Math.min(commaEnd, semicolonEnd);
        } else if (commaEnd >= 0) {
            end = commaEnd;
        } else if (semicolonEnd >= 0) {
            end = semicolonEnd;
        }
        while (end > start && /\s/.test(text.charAt(end - 1))) {
            --end;
        }

        return {
            start: start,
            end: end,
            prefix: text.substring(start, cursor).trim()
        };
    }

    function updateTopicPathSuggestions() {
        if (root.readOnly || !topicTreeField || !topicTreeField.fieldActiveFocus) {
            root.topicPathSuggestions = [];
            return;
        }

        const range = root.topicTokenRange();
        const prefix = range.prefix.toLocaleLowerCase();
        if (prefix.length === 0) {
            root.topicPathSuggestions = [];
            return;
        }

        const suggestions = [];
        for (const candidate of root.topicPathCandidates) {
            const path = candidate.path || "";
            const display = candidate.display || path;
            if (path.toLocaleLowerCase() === prefix) {
                continue;
            }
            if (path.toLocaleLowerCase().startsWith(prefix) || display.toLocaleLowerCase().startsWith(prefix)) {
                suggestions.push(candidate);
            }
            if (suggestions.length >= 8) {
                break;
            }
        }
        root.topicPathSuggestions = suggestions;
    }

    function applyTopicPathSuggestion(path) {
        if (!path || path.length === 0) {
            return;
        }

        const text = topicTreeField.text || "";
        const range = root.topicTokenRange();
        topicTreeField.text = text.substring(0, range.start) + path + text.substring(range.end);
        topicTreeField.cursorPosition = range.start + path.length;
        root.topicPathSuggestions = [];
        root.applyingTopicSuggestion = false;
        topicTreeField.forceActiveFocus();
    }

    function canImportToLibrary() {
        return root.readOnly && root.metadata && root.metadata.filename && typeof root.importToLibrary === "function";
    }

    function importIntoLibrary() {
        if (!root.canImportToLibrary() || root.importRunning) {
            return;
        }

        root.importRunning = true;
        root.importStatus = i18n("Adding book to library…");
        root.importToLibrary(function(importedEntry) {
            root.importRunning = false;
            if (importedEntry && importedEntry.filename) {
                root.readOnly = false;
                root.importStatus = i18n("Book added to library.");
                root.updateMetadata(importedEntry);
            } else {
                root.importStatus = i18n("Book could not be added to library.");
            }
        });
    }

    function coverSource() {
        const thumbnail = root.metadata.thumbnail || "";
        if (thumbnail.length === 0) {
            return "";
        }
        if (thumbnail.startsWith("file:") || thumbnail.startsWith("qrc:")) {
            return thumbnail;
        }
        return "file://" + thumbnail;
    }

    Connections {
        target: root.catalogModel()
        ignoreUnknownSignals: true

        function onSubjectCategoryModelChanged() {
            root.refreshTopicPathCandidates();
        }

        function onSubjectCategoryModelPopulatedChanged() {
            root.refreshTopicPathCandidates();
        }

        function onEntryDataUpdated() {
            root.refreshTopicPathCandidates();
        }
    }

    function checkAndRefreshBook() {
        if (root.readOnly || !root.metadata || !root.metadata.filename || root.checkRunning) {
            return;
        }

        root.checkRunning = true;
        root.checkFailed = false;
        root.checkStatus = root.checkingMessage();

        const model = root.catalogModel();
        if (!model) {
            root.checkRunning = false;
            root.checkFailed = true;
            root.checkStatus = root.checkFailedMessage();
            console.warn("Unable to refresh metadata: book list model is not available");
            return;
        }

        const window = applicationWindow();
        if (root.isPdfBook() && window && typeof window.refreshBookWithWatermarkPolicy === "function") {
            window.refreshBookWithWatermarkPolicy(root.metadata.filename, true, function(refreshedEntry, message) {
                root.checkRunning = false;
                root.checkFailed = !(refreshedEntry && refreshedEntry.filename);
                root.checkStatus = message || (root.checkFailed ? root.checkFailedMessage() : i18n("PDF metadata refreshed."));

                if (refreshedEntry && refreshedEntry.filename) {
                    root.updateMetadata(refreshedEntry);
                }
            });
            return;
        }

        const result = model.checkAndRefreshBookFromFile(root.metadata.filename);
        root.checkRunning = false;
        root.checkFailed = !(result && result.success);
        root.checkStatus = result && result.message ? result.message : root.checkFailedMessage();

        if (result && result.entry && result.entry.filename) {
            root.updateMetadata(result.entry);
        }
    }

    function checkEpub() {
        if (root.readOnly || !root.isEpubBook() || !root.metadata || !root.metadata.filename || root.epubCheckRunning) {
            return;
        }

        const model = root.catalogModel();
        if (!model || typeof model.checkEpubFile !== "function") {
            root.epubCheckFailed = true;
            root.epubCheckStatus = i18n("EPUB check is not available.");
            root.epubCheckOutput = "";
            return;
        }

        root.epubCheckRunning = true;
        root.epubCheckFailed = false;
        root.epubCheckStatus = i18n("Checking EPUB…");
        root.epubCheckOutput = "";

        Qt.callLater(() => {
            if (!root.epubCheckRunning || !root.metadata || !root.metadata.filename) {
                return;
            }

            const result = model.checkEpubFile(root.metadata.filename, root.epubCheckCommand());
            root.epubCheckRunning = false;
            root.epubCheckFailed = !(result && result.success);
            root.epubCheckStatus = result && result.message ? result.message : i18n("EPUB check failed.");
            root.epubCheckOutput = result && result.output ? result.output : "";
        });
    }

    function checkPdf() {
        if (root.readOnly || !root.isPdfBook() || !root.metadata || !root.metadata.filename || root.pdfCheckRunning) {
            return;
        }

        const model = root.catalogModel();
        if (!model || typeof model.checkPdfFile !== "function") {
            root.pdfCheckFailed = true;
            root.pdfCheckStatus = i18n("PDF check is not available.");
            root.pdfCheckReport = ({});
            root.pdfCheckRawOutput = "";
            return;
        }

        root.pdfCheckRunning = true;
        root.pdfCheckFailed = false;
        root.pdfCheckStatus = i18n("Checking PDF…");
        root.pdfCheckReport = ({});
        root.pdfCheckRawOutput = "";

        Qt.callLater(() => {
            if (!root.pdfCheckRunning || !root.metadata || !root.metadata.filename) {
                return;
            }

            const result = model.checkPdfFile(root.metadata.filename, root.pdfCheckCommand());
            root.pdfCheckRunning = false;
            root.pdfCheckFailed = !(result && result.success);
            root.pdfCheckReport = result && result.report ? result.report : ({});
            root.pdfCheckRawOutput = result && result.output ? result.output : "";
            root.pdfCheckStatus = root.pdfCheckReport.valid ? root.pdfCheckSummary(root.pdfCheckReport) : (result && result.message ? result.message : i18n("PDF check failed."));
        });
    }

    function openPdfReportDialog() {
        if (root.pdfCheckRawOutput.length === 0) {
            return;
        }

        pdfReportDialog.report = root.pdfCheckReport;
        pdfReportDialog.rawXml = root.pdfCheckRawOutput;
        pdfReportDialog.open();
    }

    function watermarkPattern() {
        return (Config.pdfWatermarkFilterPattern || Config.defaultPdfWatermarkFilterPatternValue).trim();
    }

    function clearPendingWatermarkReplacement(discardFile) {
        if (discardFile && root.pendingWatermarkReplacement.length > 0) {
            const model = root.catalogModel();
            if (model && typeof model.discardWatermarkFreePdf === "function") {
                model.discardWatermarkFreePdf(root.pendingWatermarkReplacement);
            }
        }

        root.pendingWatermarkReplacement = "";
        root.pendingWatermarkOccurrences = 0;
    }

    function createWatermarkFreePdf() {
        if (root.readOnly || !root.isPdfBook() || !root.metadata || !root.metadata.filename || root.watermarkRunning) {
            return;
        }

        const model = root.catalogModel();
        if (!model || typeof model.createWatermarkFreePdf !== "function") {
            root.watermarkStatus = i18n("PDF watermark filter is not available.");
            return;
        }

        root.clearPendingWatermarkReplacement(true);
        root.watermarkRunning = true;
        root.watermarkStatus = i18n("Searching for PDF watermark marker…");

        const result = model.createWatermarkFreePdf(root.metadata.filename, root.watermarkPattern());
        root.watermarkRunning = false;
        root.watermarkStatus = result && result.message ? result.message : i18n("PDF watermark filter failed.");

        if (result && result.filtered && result.replacementFileName) {
            root.pendingWatermarkReplacement = result.replacementFileName;
            root.pendingWatermarkOccurrences = result.occurrences || 0;
            watermarkReplacementDialog.open();
        }
    }

    function replaceWithWatermarkFreePdf() {
        if (root.pendingWatermarkReplacement.length === 0) {
            return;
        }

        const model = root.catalogModel();
        if (!model || typeof model.replaceBookFileWithWatermarkFreePdf !== "function") {
            root.watermarkStatus = i18n("PDF watermark filter is not available.");
            return;
        }

        root.watermarkRunning = true;
        root.watermarkStatus = i18n("Replacing PDF…");

        const result = model.replaceBookFileWithWatermarkFreePdf(root.metadata.filename, root.pendingWatermarkReplacement);
        root.watermarkRunning = false;
        root.watermarkStatus = result && result.message ? result.message : i18n("PDF could not be replaced.");

        if (result && result.success) {
            root.clearPendingWatermarkReplacement(false);
            if (result.entry && result.entry.filename) {
                root.updateMetadata(result.entry);
            }
        }
    }

    function setPdfVersion20() {
        if (root.readOnly || !root.isPdfBook() || !root.metadata || !root.metadata.filename || root.pdfVersionUpdating) {
            return;
        }

        const model = root.catalogModel();
        if (!model || typeof model.setPdfVersion20 !== "function") {
            root.pdfVersionStatus = i18n("PDF version update is not available.");
            return;
        }

        root.pdfVersionUpdating = true;
        root.pdfVersionStatus = i18n("Setting PDF version to 2.0…");
        const result = model.setPdfVersion20(root.metadata.filename);
        root.pdfVersionUpdating = false;
        root.pdfVersionStatus = result && result.message ? result.message : i18n("Unable to set the PDF version to 2.0.");

        if (result && result.success) {
            root.detectedBookVersion = result.version || "2.0";
            if (result.entry && result.entry.filename) {
                root.updateMetadata(result.entry);
            }
        }
    }

    title: i18nc("@info:title", "Book Details")

    Component.onCompleted: root.detectedBookVersion = root.detectBookVersion(root.metadata)

    Component.onDestruction: root.clearPendingWatermarkReplacement(true)

    Connections {
        target: root.catalogModel()

        function onEntryDataUpdated(entry) {
            if (!entry || !entry.filename || entry.filename !== root.currentMetadataFilename()) {
                return;
            }

            root.updateMetadata(entry);
        }
    }

    QQC2.Dialog {
        id: watermarkReplacementDialog

        parent: QQC2.Overlay.overlay ? QQC2.Overlay.overlay : root
        modal: true
        title: i18nc("@title:window", "Replace PDF")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel

        onAccepted: root.replaceWithWatermarkFreePdf()
        onRejected: root.clearPendingWatermarkReplacement(true)

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.largeSpacing

            QQC2.Label {
                Layout.fillWidth: true
                text: root.pendingWatermarkOccurrences === 1
                    ? i18n("One PDF watermark marker was found. Replace the original file with the watermark-free version?")
                    : i18n("%1 PDF watermark markers were found. Replace the original file with the watermark-free version?", root.pendingWatermarkOccurrences)
                wrapMode: Text.WordWrap
            }

            QQC2.Label {
                Layout.fillWidth: true
                text: root.currentMetadataFilename()
                elide: Text.ElideMiddle
            }
        }
    }

    FormCard.FormHeader {
        title: root.metadata.title
    }

    GridLayout {
        columns: root.wideLayout ? 2 : 1
        columnSpacing: Kirigami.Units.gridUnit
        rowSpacing: Kirigami.Units.largeSpacing
        Layout.fillWidth: true

        Rectangle {
            id: coverFrame

            readonly property real coverWidth: root.wideLayout ? Kirigami.Units.gridUnit * 15 : Math.min(root.width - Kirigami.Units.gridUnit * 2, Kirigami.Units.gridUnit * 15)

            Layout.preferredWidth: coverFrame.coverWidth
            Layout.preferredHeight: coverFrame.coverWidth * 1.45
            Layout.alignment: root.wideLayout ? Qt.AlignTop : Qt.AlignHCenter

            radius: Kirigami.Units.cornerRadius
            color: Kirigami.Theme.alternateBackgroundColor
            border.color: Kirigami.ColorUtils.linearInterpolation(Kirigami.Theme.textColor, Kirigami.Theme.backgroundColor, 0.75)
            border.width: 1

            Image {
                id: coverImage

                readonly property bool hasCoverSource: source.toString().length > 0

                anchors.fill: parent
                anchors.margins: Kirigami.Units.smallSpacing
                fillMode: Image.PreserveAspectFit
                source: root.coverSource()
                visible: hasCoverSource
                asynchronous: true

                sourceSize {
                    width: width
                    height: height
                }
            }

            Kirigami.Icon {
                anchors.centerIn: parent
                width: Math.min(parent.width, parent.height) * 0.55
                height: width
                source: "application-epub+zip"
                visible: coverImage.status === Image.Error || !coverImage.hasCoverSource
            }

            Image {
                id: bookTypeIcon

                readonly property bool pdfIcon: source.toString().indexOf("pdf_icon") !== -1
                readonly property real iconAspectRatio: pdfIcon ? 2 / 3 : 1
                readonly property real iconSize: Math.max(Kirigami.Units.gridUnit * 1.2, coverFrame.coverWidth * 0.16)

                width: height * iconAspectRatio
                height: iconSize
                anchors.left: coverImage.left
                anchors.top: coverImage.top
                anchors.leftMargin: Math.max(2, iconSize * 0.10)
                anchors.topMargin: Math.max(2, iconSize * 0.04)
                source: root.bookTypeIconSource()
                visible: source.toString().length > 0
                asynchronous: true
                fillMode: Image.PreserveAspectFit

                sourceSize {
                    width: Math.max(1, bookTypeIcon.width)
                    height: Math.max(1, bookTypeIcon.height)
                }
            }

            Rectangle {
                id: passwordBadge

                readonly property real iconSize: Math.max(Kirigami.Units.gridUnit * 1.2, coverFrame.coverWidth * 0.16)

                width: iconSize
                height: iconSize
                anchors.right: coverImage.right
                anchors.top: coverImage.top
                anchors.rightMargin: Math.max(2, iconSize * 0.10)
                anchors.topMargin: Math.max(2, iconSize * 0.04)
                radius: width / 2
                color: Kirigami.Theme.alternateBackgroundColor
                border.color: Kirigami.ColorUtils.linearInterpolation(Kirigami.Theme.textColor, Kirigami.Theme.backgroundColor, 0.45)
                border.width: 1
                visible: Boolean(root.metadata && root.metadata.passwordProtected)

                Kirigami.Icon {
                    anchors.fill: parent
                    anchors.margins: Math.max(2, Math.round(parent.width * 0.22))
                    source: "object-locked-symbolic"
                }
            }
        }

        FormCard.FormCard {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop

            FormCard.FormTextFieldDelegate {
                id: authorEditField

                label: i18n("Author:")
                text: root.authorText()
                placeholderText: i18n("Author name")
                enabled: root.canEditAuthor()
                visible: root.canEditAuthor()
                onAccepted: root.saveAuthor()
                onEditingFinished: root.saveAuthor()
            }

            FormCard.FormTextDelegate {
                id: authorReadOnlyField

                text: i18n("Author:")
                description: root.authorText()
                visible: !authorEditField.visible && description.length > 0
            }

            FormCard.FormDelegateSeparator {
                visible: authorEditField.visible || authorReadOnlyField.visible
            }

            FormCard.FormTextDelegate {
                id: descriptionField

                text: i18n("Description:")
                description: root.metadata.description
                visible: description.length > 0
            }

            FormCard.FormDelegateSeparator {
                visible: descriptionField.visible
            }

            FormCard.FormTextDelegate {
                id: publisherField

                text: i18n("Publisher:")
                description: root.metadata.publisher
                visible: description.length > 0
            }

            FormCard.FormDelegateSeparator {
                visible: publisherField.visible
            }

            FormCard.FormTextDelegate {
                id: languageField

                text: i18n("Language:")
                description: Qt.locale(root.metadata.language).nativeLanguageName
                visible: description.length > 0
            }

            FormCard.FormDelegateSeparator {
                visible: languageField.visible
            }

            FormCard.FormTextDelegate {
                id: bookVersionField

                text: root.bookVersionLabel()
                description: root.detectedBookVersion
                visible: description.length > 0
            }

            FormCard.FormButtonDelegate {
                id: setPdfVersion20Button

                text: i18nc("@action:button", "Set Version to PDF 2.0")
                icon.name: "document-save"
                visible: root.isPdfBook() && !root.readOnly
                enabled: !root.pdfVersionUpdating && root.catalogModel() !== null && root.metadata && root.metadata.filename && root.metadata.filename.length > 0 && root.detectedBookVersion !== "2.0"
                onClicked: root.setPdfVersion20()
            }

            FormCard.FormDelegateSeparator {
                visible: bookVersionField.visible || setPdfVersion20Button.visible
            }

            FormCard.FormTextDelegate {
                text: i18n("PDF version:")
                description: root.pdfVersionStatus
                visible: root.pdfVersionStatus.length > 0
            }

            FormCard.FormDelegateSeparator {
                visible: root.pdfVersionStatus.length > 0
            }

            FormCard.FormTextDelegate {
                id: publishingField

                text: i18n("Publishing date:")
                description: /^\d+$/.test(root.metadata.pubdate) ? root.metadata.pubdate : new Date(root.metadata.pubdate).toLocaleDateString()
                visible: description.length > 0
            }

            FormCard.FormDelegateSeparator {
                visible: publishingField.visible && copyrightField.visible
            }

            FormCard.FormTextDelegate {
                id: copyrightField
                text: i18n("Copyright:")
                description: root.metadata.rights
                visible: description.length > 0
            }

            FormCard.FormDelegateSeparator {
                visible: copyrightField.visible && locationField.visible
            }

            FormCard.FormTextDelegate {
                id: locationField
                text: i18n("Location:")
                description: root.metadata.filename
                visible: description.length > 0
                trailing: QQC2.ToolButton {
                    display: QQC2.AbstractButton.IconOnly
                    icon.name: "document-open-folder"
                    text: i18nc("@action:button", "Open Containing Folder")
                    enabled: locationField.description.length > 0
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    onClicked: Arianna.FileOpener.openContainingFolder(locationField.description)
                }
            }

            FormCard.FormDelegateSeparator {
                visible: locationField.visible && uniqueIdentifierField.visible
            }

            FormCard.FormTextDelegate {
                id: uniqueIdentifierField
                text: i18n("Unique identifier:")
                description: root.metadata.uniqueIdentifier || ""
                visible: description.length > 0
            }

            FormCard.FormDelegateSeparator {
                visible: uniqueIdentifierField.visible && identifiersField.visible
            }

            FormCard.FormTextDelegate {
                id: identifiersField
                text: i18n("Further identifiers:")
                description: root.otherIdentifiers()
                visible: description.length > 0
            }
        }
    }

    FormCard.FormHeader {
        visible: root.readOnly || root.importStatus.length > 0
        title: i18n("Library")
    }

    FormCard.FormCard {
        visible: root.readOnly || root.importStatus.length > 0
        Layout.fillWidth: true

        FormCard.FormButtonDelegate {
            visible: root.readOnly
            text: i18nc("@action:button", "Add to Library")
            icon.name: "list-add"
            enabled: root.canImportToLibrary() && !root.importRunning
            onClicked: root.importIntoLibrary()
        }

        FormCard.FormDelegateSeparator {
            visible: root.importStatus.length > 0
        }

        FormCard.FormTextDelegate {
            id: importStatusField
            text: i18n("Status:")
            description: root.importStatus
            visible: root.importStatus.length > 0
        }
    }

    FormCard.FormHeader {
        title: i18n("Topic Tree")
    }

    FormCard.FormCard {
        Layout.fillWidth: true

        FormCard.FormTextFieldDelegate {
            id: topicTreeField

            label: i18n("Topic Tree String")
            text: root.topicsText()
            placeholderText: i18n("Format: comma (,) and semicolon (;) topic separators, slash (/) level separator")
            enabled: !root.readOnly
            onTextEdited: root.updateTopicPathSuggestions()
            onCursorPositionChanged: root.updateTopicPathSuggestions()
            onFieldActiveFocusChanged: {
                if (fieldActiveFocus) {
                    root.refreshTopicPathCandidates();
                } else if (!root.applyingTopicSuggestion) {
                    root.topicPathSuggestions = [];
                }
            }
            onAccepted: {
                if (root.topicPathSuggestions.length > 0) {
                    root.applyingTopicSuggestion = true;
                    root.applyTopicPathSuggestion(root.topicPathSuggestions[0].path);
                } else {
                    root.saveTopics();
                }
            }
            Keys.onTabPressed: event => {
                if (root.topicPathSuggestions.length === 0) {
                    event.accepted = false;
                    return;
                }

                root.applyingTopicSuggestion = true;
                root.applyTopicPathSuggestion(root.topicPathSuggestions[0].path);
                event.accepted = true;
            }
            onEditingFinished: {
                if (!root.applyingTopicSuggestion) {
                    root.saveTopics();
                }
            }
        }

        FormCard.FormDelegateSeparator {
            visible: root.topicPathSuggestions.length > 0
        }

        ColumnLayout {
            visible: root.topicPathSuggestions.length > 0
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.topMargin: Kirigami.Units.smallSpacing
            Layout.bottomMargin: Kirigami.Units.smallSpacing
            spacing: 0

            Repeater {
                model: root.topicPathSuggestions

                delegate: QQC2.ItemDelegate {
                    required property var modelData

                    Layout.fillWidth: true
                    text: modelData.display
                    icon.name: "tag-symbolic"
                    onPressedChanged: if (pressed) {
                        root.applyingTopicSuggestion = true;
                    }
                    onClicked: root.applyTopicPathSuggestion(modelData.path)

                    QQC2.ToolTip.visible: hovered && modelData.path !== modelData.display
                    QQC2.ToolTip.text: modelData.path
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }
            }
        }

        FormCard.FormDelegateSeparator {
            visible: !root.readOnly
        }

        FormCard.FormButtonDelegate {
            visible: !root.readOnly
            text: i18n("Save Topic Tree")
            icon.name: "document-save"
            enabled: root.catalogModel() !== null && root.metadata && root.metadata.filename && root.leafTopicList(topicTreeField.text).join(", ") !== root.topicsText()
            onClicked: root.saveTopics()
        }
    }

    FormCard.FormHeader {
        visible: !root.readOnly
        title: root.maintenanceTitle()
    }

    FormCard.FormCard {
        visible: !root.readOnly
        Layout.fillWidth: true

        FormCard.FormButtonDelegate {
            id: watermarkFilterButton

            visible: root.isPdfBook()
            text: i18nc("@action:button", "Remove PDF Watermark")
            icon.name: "edit-clear"
            enabled: !root.watermarkRunning && root.catalogModel() !== null && root.metadata && root.metadata.filename && root.metadata.filename.length > 0
            onClicked: root.createWatermarkFreePdf()
        }

        FormCard.FormButtonDelegate {
            id: epubCheckButton

            visible: root.isEpubBook()
            text: i18nc("@action:button", "Check EPUB")
            icon.name: "document-preview"
            enabled: !root.epubCheckRunning && root.catalogModel() !== null && root.metadata && root.metadata.filename && root.metadata.filename.length > 0
            onClicked: root.checkEpub()
        }

        FormCard.FormButtonDelegate {
            id: pdfCheckButton

            visible: root.isPdfBook()
            text: i18nc("@action:button", "Check PDF")
            icon.name: "document-preview"
            enabled: !root.pdfCheckRunning && root.catalogModel() !== null && root.metadata && root.metadata.filename && root.metadata.filename.length > 0
            onClicked: root.checkPdf()
        }

        FormCard.FormDelegateSeparator {
            visible: watermarkFilterButton.visible || epubCheckButton.visible || pdfCheckButton.visible
        }

        FormCard.FormButtonDelegate {
            text: i18n("Check and Refresh Metadata")
            icon.name: "view-refresh"
            enabled: !root.checkRunning && root.catalogModel() !== null && root.metadata && root.metadata.filename && root.metadata.filename.length > 0
            onClicked: root.checkAndRefreshBook()
        }

        FormCard.FormDelegateSeparator {
            visible: watermarkStatusField.visible || checkStatusField.visible || epubCheckStatusField.visible || pdfCheckStatusField.visible
        }

        FormCard.FormTextDelegate {
            id: watermarkStatusField
            text: i18n("Status:")
            description: root.watermarkStatus
            visible: root.watermarkStatus.length > 0
        }

        FormCard.FormDelegateSeparator {
            visible: watermarkStatusField.visible && checkStatusField.visible
        }

        FormCard.FormTextDelegate {
            id: checkStatusField
            text: i18n("Status:")
            description: root.checkStatus
            visible: root.checkStatus.length > 0
        }

        FormCard.FormDelegateSeparator {
            visible: (watermarkStatusField.visible || checkStatusField.visible) && epubCheckStatusField.visible
        }

        FormCard.FormTextDelegate {
            id: epubCheckStatusField
            text: i18n("EPUB check:")
            description: root.epubCheckStatus
            visible: root.epubCheckStatus.length > 0
        }

        FormCard.FormDelegateSeparator {
            visible: (watermarkStatusField.visible || checkStatusField.visible || epubCheckStatusField.visible) && pdfCheckStatusField.visible
        }

        FormCard.FormTextDelegate {
            id: pdfCheckStatusField
            text: i18n("PDF check:")
            description: root.pdfCheckStatus
            visible: root.pdfCheckStatus.length > 0
        }

        FormCard.FormDelegateSeparator {
            visible: pdfCheckStatusField.visible && pdfReportButton.visible
        }

        FormCard.FormButtonDelegate {
            id: pdfReportButton

            visible: root.pdfCheckRawOutput.length > 0
            text: i18nc("@action:button", "Show PDF Validation Report")
            icon.name: "document-preview"
            onClicked: root.openPdfReportDialog()
        }

        FormCard.FormDelegateSeparator {
            visible: root.epubCheckOutput.length > 0
        }

        ColumnLayout {
            visible: root.epubCheckOutput.length > 0
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.topMargin: Kirigami.Units.smallSpacing
            Layout.bottomMargin: Kirigami.Units.smallSpacing

            QQC2.Label {
                text: i18n("EPUB check output:")
                font.bold: true
            }

            QQC2.ScrollView {
                id: epubCheckOutputScrollView

                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(Kirigami.Units.gridUnit * 18,
                    Math.max(Kirigami.Units.gridUnit * 8, epubCheckOutputArea.contentHeight + epubCheckOutputArea.topPadding + epubCheckOutputArea.bottomPadding))
                clip: true
                QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff
                QQC2.ScrollBar.vertical.policy: QQC2.ScrollBar.AsNeeded

                QQC2.TextArea {
                    id: epubCheckOutputArea

                    width: epubCheckOutputScrollView.availableWidth
                    text: root.epubCheckOutputMarkup()
                    textFormat: TextEdit.RichText
                    readOnly: true
                    selectByMouse: true
                    wrapMode: Text.Wrap
                }
            }
        }

    }

    ReportDialog {
        id: pdfReportDialog

        parent: QQC2.Overlay.overlay ? QQC2.Overlay.overlay : root
    }
}
