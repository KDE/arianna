import QtQuick 2.15
import QtQuick.Controls 2.15 as QQC2
import QtWebEngine 1.4
import QtWebChannel 1.4
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.13 as Kirigami
import org.kde.arianna 1.0

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
        property var cachedLocations: Cache.loadLocations()
        onCachedLocationsChanged: Cache.saveLocations(cachedLocations)
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

        WebEngineView {
            id: view
            anchors.fill: parent
            url: "main.html"
            webChannel: channel
            onJavaScriptConsoleMessage: console.error(message)

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
