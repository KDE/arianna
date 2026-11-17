// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Dialog {
    id: root
    objectName: "crossReferenceDialog"
    parent: QQC2.Overlay.overlay
    anchors.centerIn: parent
    property string serverUrl: ""
    property string sessionToken: ""
    property string sourceBookId: ""
    property var referenceContext: ({})
    property bool canEdit: true
    property bool confirmingDeletion: false
    property string errorMessage: ""
    signal editRequested(var context)
    signal openRequested(var context)
    signal removed(var response)
    title: (confirmingDeletion ? i18n("Delete Reference?") : i18n("Reference"))
        + (referenceContext.sourceAnchorTitle ? " — " + referenceContext.sourceAnchorTitle : "")
    modal: true
    standardButtons: QQC2.Dialog.NoButton
    padding: Kirigami.Units.largeSpacing
    closePolicy: deleteRequest.running ? QQC2.Popup.NoAutoClose : QQC2.Popup.CloseOnEscape
    showCloseButton: !deleteRequest.running
    width: Math.min(Kirigami.Units.gridUnit * 44, parent ? parent.width - 24 : 800)
    height: Math.min(Kirigami.Units.gridUnit * (confirmingDeletion ? 18 : 32), parent ? parent.height - 24 : 650)

    function showReference(bookId, context, deletion) {
        if (deleteRequest.running) return;
        sourceBookId = bookId;
        referenceContext = context;
        confirmingDeletion = !!deletion;
        errorMessage = "";
        open();
    }
    function remove() {
        if (!canEdit || deleteRequest.running) return;
        deleteRequest.send("DELETE", "/" + encodeURIComponent(sourceBookId) + "/ref/" + encodeURIComponent(referenceContext.ref), null, 60000);
    }
    BookServerRequest {
        id: deleteRequest
        baseUrl: root.serverUrl; sessionToken: root.sessionToken
        onFinished: (response, error) => {
            if (error) { root.errorMessage = error.message; return; }
            root.close();
            root.removed(response);
        }
    }
    contentItem: ColumnLayout {
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: !!root.referenceContext.targetNavigationError
            text: root.referenceContext.targetNavigationError || ""
            type: Kirigami.MessageType.Error
        }
        TargetAnchorRepair {
            Layout.fillWidth: true
            visible: root.canEdit && !root.confirmingDeletion && !!repair.fileName
            repair: root.referenceContext.targetAnchorRepair || ({})
            enabled: !deleteRequest.running
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: !!root.errorMessage
            text: root.errorMessage
            type: Kirigami.MessageType.Error
        }
        QQC2.Label {
            Layout.fillWidth: true
            visible: root.confirmingDeletion
            text: i18n("This reference will be removed from the database and its source anchor from the EPUB. The visible text will be kept.")
            wrapMode: Text.Wrap
        }
        Loader {
            Layout.fillWidth: true; Layout.fillHeight: true
            active: root.visible && !root.confirmingDeletion
            visible: active
            sourceComponent: NoteHtmlView {
                html: {
                    const text = root.referenceContext.targetPreviewHtml || i18n("No preview available");
                    return /<[^>]+>/.test(text) ? text : text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
                }
            }
        }
        Flow {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            QQC2.BusyIndicator { visible: deleteRequest.running; running: visible; Layout.preferredWidth: Kirigami.Units.gridUnit * 2; Layout.preferredHeight: width }
            QQC2.Button {
                text: root.confirmingDeletion ? i18n("Cancel") : i18n("Close")
                enabled: !deleteRequest.running
                onClicked: root.close()
            }
            QQC2.Button {
                objectName: "crossReferenceOpen"
                visible: !root.confirmingDeletion
                text: i18n("Open Reference")
                enabled: !!root.referenceContext.targetNavigationLocation
                onClicked: { root.close(); root.openRequested(root.referenceContext); }
            }
            QQC2.Button {
                objectName: "crossReferenceEdit"
                visible: !root.confirmingDeletion && root.canEdit
                text: i18n("Edit Reference")
                icon.name: "document-edit"
                onClicked: { root.close(); root.editRequested(root.referenceContext); }
            }
            QQC2.Button {
                objectName: "crossReferenceDelete"
                visible: root.canEdit
                text: i18n("Delete Reference")
                icon.name: "edit-delete"
                enabled: !deleteRequest.running
                onClicked: {
                    if (root.confirmingDeletion) root.remove();
                    else root.confirmingDeletion = true;
                }
            }
        }
    }
}
