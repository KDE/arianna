import QtQuick 2.6
import QtQuick.Controls 2.0 as Controls
import QtWebEngine 1.4
import QtWebChannel 1.4
import QtQuick.Layouts 1.2
import org.kde.kirigami 2.13 as Kirigami
import org.kde.plasma.components 3.0 as PC3

Kirigami.ApplicationWindow {
    id: root

    title: "Arianna"
    width: Kirigami.Units.gridUnit * 30
    height: Kirigami.Units.gridUnit * 40
    minimumWidth: Kirigami.Units.gridUnit * 20
    minimumHeight: Kirigami.Units.gridUnit * 30

    QtObject {
        id: backend
        WebChannel.id: "backend"
        property double progress: 0
        property bool locationsReady: false
        property var cachedLocations: py.loadLocations()

        onCachedLocationsChanged: py.saveLocations(cachedLocations)
    }

    WebChannel {
        id: channel
        registeredObjects: [backend]
    }

    pageStack.initialPage: Kirigami.Page {

        title: "Title goes here"
        padding: 0

        actions {
            right: Kirigami.Action {
                text: "Next Page"
                icon.name: "arrow-right"
                shortcut: Qt.LeftArrow
                onTriggered: view.next()
            }
            left: Kirigami.Action {
                text: "Previous Page"
                icon.name: "arrow-left"
                onTriggered: view.runJavaScript('test()')
            }
        }

        WebEngineView {
            id: view
            anchors.fill: parent
            url: "main.html"
            webChannel: channel
            onJavaScriptConsoleMessage: {console.log(message)}

            function next() {view.runJavaScript('rendition.next()')}
            function prev() {view.runJavaScript('rendition.prev()')}

        }

    }
    footer: PC3.Slider {
        padding: Kirigami.Units.smallSpacing
        visible: backend.locationsReady
        value: backend.progress
        onValueChanged: backend.progress = value
        live: false
    }
}
