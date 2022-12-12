import QtQuick 2.15
import QtQuick.Controls 2.15 as QQC2
import QtWebEngine 1.4
import QtWebChannel 1.4
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.13 as Kirigami
import org.kde.arianna 1.0
import Qt.labs.platform 1.1

Kirigami.ApplicationWindow {
    id: root

    title: "Arianna"
    width: Kirigami.Units.gridUnit * 30
    height: Kirigami.Units.gridUnit * 40
    minimumWidth: Kirigami.Units.gridUnit * 20
    minimumHeight: Kirigami.Units.gridUnit * 30

    ListModel {
        id: searchResultModel
    }

    QtObject {
        id: backend
        WebChannel.id: "backend"
        property var findResults: ({})
        property double progress: 0
        property bool locationsReady: false
        property var cachedLocations: Cache.loadLocations()
        property var metadata: null
        property var top: ({})
        onCachedLocationsChanged: Cache.saveLocations(cachedLocations)
        function get(script, callback) {
            return view.runJavaScript(`JSON.stringify(${script})`, callback)
        }
        onMetadataChanged: {
            if (metadata) {
                view.runJavaScript('loadLocations()')
                view.runJavaScript('render()')
            }
        }
        function dispatch(action) {
            switch (action.type) {
            case 'book-ready':
                searchResultModel.clear();
                get('book.package.metadata', metadata => {
                    backend.metadata = metadata;
                });
                get('book.navigation.toc', toc => {
                    backend.toc = toc;
                })
                break;
            case 'rendition-ready':
                view.runJavaScript('setupRendition()');
                setStyle();
                break;
            case 'locations-ready':
                backend.locationsReady = true;
                break;
            case 'locations-generated':
                backend.locationsReady = true;
                get('book.key()', key => {
                    const cachedLocations = backend.cachedLocations;
                    cachedLocations[key] = action.payload;
                    backend.cachedLocations = cachedLocations;
                });
                break;
            case 'book-error':
                console.error('Book error', action.payload);
                view.file = '';
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
            }
            console.error(action.type)
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
                margin: 0,
                maxWidth: Config.maxWidth,
                usePublisherFont: Config.usePublisherFont,
                justify: Config.justify,
                hyphenate: Config.hyphenate,
                fgColor: Kirigami.Theme.textColor.toString(),
                bgColor: Kirigami.Theme.backgroundColor.toString(),
                linkColor: Kirigami.Theme.linkColor.toString(),
                invert: false,
                brightness: Config.brightness,
                skeuomorphism: Config.skeuomorphism,
                ibooksInternalTheme: getIbooksInternalTheme(Kirigami.Theme.backgroundColor)
            }

            view.runJavaScript(`setStyle(${JSON.stringify(style)})`)
        }
    }

    WebChannel {
        id: channel
        registeredObjects: [backend]
    }

    pageStack.initialPage: Kirigami.Page {
        title: "Ebook Title"
        padding: 0
        actions {
            right: Kirigami.Action {
                text: i18n("Next Page")
                icon.name: "arrow-right"
                shortcut: Qt.LeftArrow
                onTriggered: view.next()
            }
            left: Kirigami.Action {
                text: i18n("Previous Page")
                icon.name: "arrow-left"
                onTriggered: view.prev()
            }
        }

        titleDelegate: Kirigami.SearchField {
            id: searchField
            autoAccept: false
            visible: view.file !== ''
            onAccepted: if (text === '') {
                view.runJavaScript(`find.clearHighlight()`)
            } else {
                view.runJavaScript(`find.find('${text}', true, true)`);
                popup.open();
            }
            selectByMouse: true
            property alias popup: popup

            QQC2.Popup {
                padding: 1
                id: popup
                x: searchField.y
                y: searchField.y + searchField.height
                width: Kirigami.Units.gridUnit * 15
                height: Kirigami.Units.gridUnit * 20

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

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            text: i18n("No book selected")
            visible: view.file === ''
            helpfulAction: Kirigami.Action {
                text: i18n("Open file")
                onTriggered: {
                    const fileDialog = openFileDialog.createObject(QQC2.ApplicationWindow.overlay)
                    fileDialog.accepted.connect(() => {
                        const file = fileDialog.file;
                        if (!file) {
                            return;
                        }
                        view.file = file;
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
            property string file: ''
            anchors.fill: parent
            url: "main.html"
            visible: file !== ''
            onFileChanged: {
                const renderTo = layouts['auto'].renderTo;
                const options = JSON.stringify(layouts['auto'].options);
                view.runJavaScript(`open('${file}', "filename.epub", "epub", ${renderTo}, '${options}')`);
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
            webChannel: channel
            onJavaScriptConsoleMessage: console.error('WEB:', message, lineNumber, sourceID)

            function next() {view.runJavaScript('rendition.next()')}
            function prev() {view.runJavaScript('rendition.prev()')}
        }

    }
    footer: QQC2.Slider {
        padding: Kirigami.Units.smallSpacing
        visible: backend.locationsReady
        value: backend.progress
        onValueChanged: backend.progress = value
        live: false
    }
}
