// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// One cancellable read or explicitly submitted write over the existing HTTP API.
QtObject {
    id: root
    property string baseUrl: ""
    property string sessionToken: ""
    property bool running: false
    property var pending: null
    property int generation: 0
    property Timer deadline: Timer {
        onTriggered: root.finish(null, {code: "timeout", message: i18n("The BookServer request timed out.")}, true)
    }
    signal finished(var response, var error)

    function cancel() {
        ++generation;
        deadline.stop();
        const previous = pending;
        pending = null;
        running = false;
        if (previous) previous.abort();
    }

    function finish(response, error, abortRequest = false) {
        if (!running) return;
        ++generation;
        deadline.stop();
        const previous = pending;
        pending = null;
        running = false;
        if (abortRequest && previous) previous.abort();
        finished(response, error);
    }

    function send(method, path, body = null, timeout = 15000) {
        cancel();
        running = true;
        if (!baseUrl || !sessionToken) {
            finish(null, {code: "network", message: i18n("The book server is unavailable.")});
            return;
        }
        const serial = generation;
        const request = new XMLHttpRequest();
        pending = request;
        request.open(method, baseUrl + path);
        request.setRequestHeader("X-Arianna-Session-Token", sessionToken);
        if (body !== null) request.setRequestHeader("Content-Type", "application/json");
        request.onreadystatechange = function() {
            if (serial !== generation || request.readyState !== XMLHttpRequest.DONE) return;
            let response = null;
            try { response = request.responseText ? JSON.parse(request.responseText) : {}; }
            catch (error) {
                if (request.status >= 200 && request.status < 300) {
                    finish(null, {code: "json", message: i18n("BookServer returned invalid data.")});
                    return;
                }
            }
            if (request.status >= 200 && request.status < 300) finish(response, null);
            else finish(null, {code: request.status ? "http" : "network", status: request.status,
                responseJson: response, message: response?.message || (request.status
                    ? i18n("BookServer request failed (HTTP %1).", request.status)
                    : i18n("No connection to BookServer."))});
        };
        deadline.interval = timeout;
        deadline.start();
        request.send(body === null ? "" : JSON.stringify(body));
    }
    Component.onDestruction: cancel()
}
