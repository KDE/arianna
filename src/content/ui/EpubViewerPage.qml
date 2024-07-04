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
    readonly property color readerTheme: Kirigami.Theme.backgroundColor
    readonly property bool hideSidebar: true

    property var layouts: {
        'auto': {
            'renderTo': "'viewer'",
            'options': { width: '100%', flow: 'paginated', maxSpreadColumns: 2 }
        },
        'single': {
            renderTo: "'viewer'",
            options: { width: '100%', flow: 'paginated', spread: 'none' }
        },
        'scrolled': {
            renderTo: 'document.body',
            options: { width: '100%', flow: 'scrolled-doc' },
        },
        'continuous': {
            renderTo: 'document.body',
            options: { width: '100%', flow: 'scrolled', manager: 'continuous' },
        }
    }

    signal relocated(newLocation: var, newProgress: int)
    signal locationsLoaded(locations: var)
    signal bookReady(title: var)
    signal bookClosed()

    function reloadBook() {
        if (!root.url || view.loading) {
            return;
        }
        const renderTo = layouts['auto'].renderTo;
        const options = JSON.stringify(layouts['auto'].options);
        const urlNormalized = JSON.stringify('http://127.0.0.1:45961/book?url=' + encodeURIComponent(root.url));
        view.runJavaScript(`openSync(${urlNormalized})`);
    }

    title: backend.metadata ? backend.metadata.title : ''
    padding: 0

    Keys.onLeftPressed: view.prev()
    Keys.onRightPressed: view.next()

    onUrlChanged: reloadBook()
    onReaderThemeChanged: backend.setStyle()

    Kirigami.SearchDialog {
        id: searchDialog

        onAccepted: if (text === '') {
            view.runJavaScript(`find.clearHighlight()`)
        } else {
            searchResultModel.search(text);
            searchResultModel.loading = true;
        }

        emptyText: if (searchResultModel.loading) {
            return i18n("Loading");
        } else if (searchResultsCount === 0 && searchDialog.text.length > 2) {
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

            onClicked: view.runJavaScript(`rendition.display('${cfi}')`)

            contentItem: ColumnLayout {
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
        }

        Shortcut {
            sequence: "Ctrl+K"
            onActivated: searchDialog.open()
        }
    }

    actions: [
        Kirigami.Action {
            text: i18nc("@action:intoolbar", "Search")
            displayHint: Kirigami.DisplayHint.IconOnly
            icon.name: "system-search-symbolic"
            onTriggered: searchDialog.open();
        },
        Kirigami.Action {
            text: i18n("Book Details")
            displayHint: Kirigami.DisplayHint.IconOnly
            icon.name: "documentinfo"
            enabled: backend.metadata
            onTriggered: {
                applicationWindow().pageStack.pushDialogLayer(Qt.resolvedUrl("./BookDetailsPage.qml"), {
                    metadata: backend.metadata,
                })
            }
        }
    ]

    SearchModel {
        id: searchResultModel

        onSearchTriggered: (text) => {
            view.runJavaScript(`find.find('${text}', true, true)`)
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
                const fileDialog = openFileDialog.createObject(QQC2.ApplicationWindow.overlay)
                fileDialog.accepted.connect(() => {
                    const file = fileDialog.file;
                    if (!file) {
                        return;
                    }
                    root.url = file;
                })
                fileDialog.open();
            }
        }
    }

    Component {
        id: openFileDialog

        FileDialog {
            id: root
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

    WebEngineView {
        id: view
        anchors.fill: parent
        url: Qt.resolvedUrl("main.html")
        visible: root.url !== ''
        webChannel: channel

        onVisibleChanged: if (!visible) {
            root.bookClosed();
        }

        onJavaScriptConsoleMessage: (level, message, lineNumber, sourceID) => {
            console.error('WEB:', level, message, lineNumber, sourceID)
        }
        onLoadingChanged: reloadBook()

        function next() {
            view.runJavaScript('rendition.next()');
        }

        function prev() {
            view.runJavaScript('rendition.prev()');
        }

        function goTo(cfi) {
            view.runJavaScript('rendition.display("' + cfi + '")');
        }

        QQC2.Menu {
            id: selectionPopup
            Connections {
                target: backend
                function onSelectionChanged() {
                    selectionPopup.popup()
                }
            }

            QQC2.MenuItem {
                text: i18n("Copy")
                icon.name: 'edit-copy'
                onClicked: Clipboard.saveText(backend.selection.text)
            }

            QQC2.MenuItem {
                text: i18n("Find")
                icon.name: 'search'
                onClicked: view.runJavaScript(`find.find('${backend.selection.text}', true, true)`);
            }
        }
    }
    footer: QQC2.ToolBar {
        visible: backend.locationsReady
        contentItem: RowLayout {
            QQC2.ToolButton {
                id: progressButton
                text: i18nc("Book reading progress", "%1%", Math.round(backend.progress * 100))
                onClicked: menu.popup(progressButton, 0, - menu.height)

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
                                text: backend.location.timeInChapter ? Format.formatDuration(backend.location.timeInChapter) : i18n("Loading")
                            }
                            QQC2.Label {
                                Kirigami.FormData.label: i18n("Time left in book:")
                                text: backend.location.timeInBook ? Format.formatDuration(backend.location.timeInBook) : i18n("Loading")
                            }
                        }
                    }
                }
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
                onValueChanged: backend.progress = value
                live: false
                Layout.fillWidth: true
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
        property double progress: 0
        property var location
        property var locations: root.locations
        property bool locationsReady: false
        property var metadata: null
        property var top: ({})
        property string file: root.url
        function get(script, callback) {
            return view.runJavaScript(`JSON.stringify(${script})`, callback)
        }
        onMetadataChanged: if (metadata) {
            view.runJavaScript('loadLocations()', () => {
                view.runJavaScript('render()')
            });
        }
        function dispatch(action) {
            switch (action.type) {
            case 'book-ready':
                console.log(JSON.stringify(action.payload.book));
                searchResultModel.clear();
                searchResultModel.loading = false;
                
                const { book } = action.payload;
                if (book && book.toc) {
                    applicationWindow().contextDrawer.model.importFromJson(JSON.stringify(book.toc));
                } else {
                    console.warn('Book or TOC not available');
                }

                const metadata = action.payload.book.metadata;
                if (metadata) {
                    backend.metadata = metadata;
                    root.bookReady(backend.metadata.title);
                    Database.addBook(backend.file, JSON.stringify(metadata));
                }
                // get('book.navigation.toc', toc => {
                //     applicationWindow().contextDrawer.model.importFromJson(toc)
                // });
                break;
            case 'rendition-ready':
                setStyle();
                view.runJavaScript('setupRendition()');
                if (currentLocation) {
                    view.runJavaScript(`rendition.display('${currentLocation}')`)
                } else {
                    view.runJavaScript(`rendition.display()`);
                }
                break;
            case 'locations-ready':
                backend.locationsReady = true;
                break;
            case 'locations-generated':
                backend.locationsReady = true;
                root.locationsLoaded(action.payload.locations)
                break;
            case 'book-error':
                console.error('Book error', action.payload);
                break;
            case 'selection':
                backend.selection = action.payload;
                break;
            case 'relocated':
                root.relocated(action.payload.start.cfi, action.payload.start.percentage * 100)
                backend.location = action.payload;
                break;
            case 'find-results':
                searchResultModel.resultFound(action.payload.q, action.payload.results);
                break;
            }
        }

        function setStyle() {
            const getIbooksInternalTheme = bgColor => {
                const red = bgColor.r;
                const green = bgColor.g;
                const blue = bgColor.b;
                const l = 0.299 * red + 0.587 * green + 0.114 * blue;
                if (l < 0.3) return 'Night';
                else if (l < 0.7) return 'Gray';
                else if (red > green && green > blue) return 'Sepia';
                else return 'White';
            }
            const fontDesc = Config.defaultFont;
            const fontFamily = fontDesc.family
            const fontSizePt = fontDesc.pointSize
            const fontSize = fontDesc.pixelSize
            let fontWeight = 400
            const fontStyle = fontDesc.styleName

            // unfortunately, it appears that WebKitGTK doesn't support font-stretch
            const fontStretch = [
                'ultra-condensed', 'extra-condensed', 'condensed', 'semi-condensed', 'normal',
                'semi-expanded', 'expanded', 'extra-expanded', 'ultra-expanded'
            ][fontDesc.stretch]

            const style = {
                fontFamily: fontFamily,
                fontSize: fontSize,
                fontWeight: fontWeight,
                fontStyle: fontStyle,
                fontStretch: fontStretch,
                spacing: Config.spacing,
                margin: Config.margin,
                maxWidth: Config.maxWidth,
                usePublisherFont: Config.usePublisherFont,
                justify: Config.justify,
                hyphenate: Config.hyphenate,
                fgColor: Kirigami.Theme.textColor.toString(),
                bgColor: Kirigami.Theme.backgroundColor.toString(),
                linkColor: Kirigami.Theme.linkColor.toString(),
                selectionFgColor: Kirigami.Theme.highlightedTextColor.toString(),
                selectionBgColor: Kirigami.Theme.highlightColor.toString(),
                invert: Config.invert,
                brightness: Config.brightness,
                ibooksInternalTheme: getIbooksInternalTheme(Kirigami.Theme.backgroundColor)
            }

            view.runJavaScript(`setStyle(${JSON.stringify(style)})`)
        }
    }

    WebChannel {
        id: channel
        registeredObjects: [backend]
    }
}

