// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick 2.15
import QtQuick.Controls 2.15 as QQC2
import QtWebEngine 1.4
import QtWebChannel 1.4
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.13 as Kirigami
import org.kde.arianna 1.0
import Qt.labs.platform 1.1

Kirigami.Page {
    id: root

    property var url
    property string filename
    property var locations
    property var currentLocation

    signal relocated(newLocation: var, newProgress: int)
    signal locationsLoaded(locations: var)
    signal bookReady(title: var)
    signal bookClosed()

    title: backend.metadata ? backend.metadata.title : ''

    padding: 0

    onUrlChanged: {
        if (!url || view.loading) {
            return;
        }
        const renderTo = layouts['auto'].renderTo;
        const options = JSON.stringify(layouts['auto'].options);
        view.runJavaScript(`open('${root.url}', "filename.epub", "epub", ${renderTo}, '${options}')`);
    }

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

    ListModel {
        id: searchResultModel
    }

    actions.main: Kirigami.Action {
        displayComponent: Kirigami.SearchField {
            id: searchField
            autoAccept: false
            visible: view.file !== ''
            onAccepted: if (text === '') {
                view.runJavaScript(`find.clearHighlight()`)
            } else {
                view.runJavaScript(`find.find('${text}', true, true)`);
                popup.open()
            }
            selectByMouse: true
            property alias popup: popup

            QQC2.Popup {
                padding: 1
                id: popup
                x: searchField.width - width
                y: searchField.y + searchField.height
                width: Kirigami.Units.gridUnit * 15
                height: Kirigami.Units.gridUnit * 20
                Connections {
                    target: searchResultModel
                    function onCountChanged() {
                        if (searchResultModel.count > 0) {
                            console.error('count changed', searchResultModel.count)
                            searchField.popup.open()
                        }
                    }
                }

                contentItem: ColumnLayout {
                    width: popup.width
                    spacing: 0

                    QQC2.ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        contentItem: ListView {
                            model: searchResultModel

                            delegate: QQC2.ItemDelegate {
                                width: ListView.view.width
                                Kirigami.Theme.colorSet: Kirigami.Theme.Window
                                Kirigami.Theme.inherit: false
                                onClicked: view.runJavaScript(`rendition.display('${cfi}')`)
                                contentItem: ColumnLayout {
                                    QQC2.Label {
                                        Layout.fillWidth: true
                                        text: model.sectionMarkup
                                        wrapMode: Text.WordWrap
                                        font: Kirigami.Theme.smallFont
                                    }
                                    QQC2.Label {
                                        Layout.fillWidth: true
                                        text: model.markup
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }
                        }
                    }
                }
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
            title: i18n("Please choose a file")
        }
    }

    WebEngineView {
        id: view
        anchors.fill: parent
        url: "main.html"
        visible: root.url !== ''
        webChannel: channel
        onJavaScriptConsoleMessage: console.error('WEB:', message, lineNumber, sourceID)

        function next() {
            view.runJavaScript('rendition.next()');
        }
        function prev() {
            view.runJavaScript('rendition.prev()');
        }

        onVisibleChanged: {
            if(!visible)
                root.bookClosed();
        }

        onLoadingChanged: {
            if (!root.url || view.loading) {
                return;
            }
            const renderTo = root.layouts['auto'].renderTo;
            const options = JSON.stringify(root.layouts['auto'].options);
            view.runJavaScript(`open('${root.url}', "filename.epub", "epub", ${renderTo}, '${options}')`);
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

                Component.onCompleted: if (background.hasOwnProperty("showMenuArrow")) {
                    background.showMenuArrow = true;
                }
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
        property var findResults: ({})
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
                searchResultModel.clear();
                get('book.package.metadata', metadata => {
                    backend.metadata = JSON.parse(metadata);
                    root.bookReady(backend.metadata.title);
                    Database.addBook(backend.file, metadata);
                });
                get('book.navigation.toc', toc => {
                    backend.toc = toc;
                });
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
                view.file = '';
                break;
            case 'selection':
                backend.selection = action.payload;
                break;
            case 'relocated':
                root.relocated(action.payload.start.cfi, action.payload.start.percentage * 100)
                backend.location = action.payload;
                break;
            case 'find-results':
                const q  = action.payload.q;
                const results  = action.payload.results;
                searchResultModel.clear();
                var markupEscape = text => text ? text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&#039;") : ''
                var regexEscape = str => str ? str.replace(/[.*+\-?^${}()|[\]\\]/g, '\\$&') : ''
                const regex = new RegExp(regexEscape(q), 'ig')
                results.forEach(({ cfi, excerpt, section }) => {
                    const text = markupEscape(excerpt.trim().replace(/\n/g, ' '))
                    const markup = text.replace(regex, `<b>${regex.exec(text)[0]}</b>`)
                    const sectionMarkup = `<span alpha="50%" size="smaller">${
                        markupEscape(section)}</span>`

                    searchResultModel.append({cfi: cfi, markup: markup, sectionMarkup: sectionMarkup })
                })
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
                invert: false,
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

