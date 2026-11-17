// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtWebEngine
Item {
    id: root
    property string html: ""
    function updateContent() {
        if (browser.url.toString() === Qt.resolvedUrl('note-view.html').toString() && !browser.loading)
            browser.runJavaScript('globalThis.showNote?.(' + JSON.stringify(root.html) + ')');
    }
    onHtmlChanged: updateContent()
    WebEngineProfile {
        id: noteProfile
        offTheRecord: true
        httpCacheType: WebEngineProfile.MemoryHttpCache
        httpCacheMaximumSize: 16 * 1024 * 1024
        persistentCookiesPolicy: WebEngineProfile.NoPersistentCookies
    }
    WebEngineView {
        id: browser
        objectName: "noteHtmlBrowser"
        anchors.fill: parent
        profile: noteProfile
        url: Qt.resolvedUrl('note-view.html')
        settings.localContentCanAccessFileUrls: false
        settings.localContentCanAccessRemoteUrls: false
        settings.javascriptCanAccessClipboard: false
        onLoadingChanged: request => { if (request.status === WebEngineView.LoadSucceededStatus) root.updateContent(); }
        onNavigationRequested: request => {
            if (request.isMainFrame && request.url.toString() !== Qt.resolvedUrl('note-view.html').toString()) {
                request.action = WebEngineView.IgnoreRequest;
                if (request.navigationType === WebEngineView.LinkClickedNavigation && /^https?:/.test(request.url.toString()))
                    Qt.openUrlExternally(request.url);
            }
        }
        onNewWindowRequested: request => {}
        onPermissionRequested: permission => permission.deny()
    }
}
