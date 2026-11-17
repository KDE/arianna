// SPDX-License-Identifier: GPL-2.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

QQC2.Dialog {
    id: root
    objectName: "bookFormatDialog"
    property var variants: []
    property string bookTitle: ""
    signal bookChosen(var entry)
    title: i18nc("@title:window", "Choose Book Format")
    modal: true
    anchors.centerIn: parent
    width: Math.min(Kirigami.Units.gridUnit * 30, parent ? parent.width - Kirigami.Units.largeSpacing * 2 : 500)
    standardButtons: QQC2.Dialog.Cancel

    function openBook(entry, modifiers) {
        const formats = entry.formatVariants || [];
        if ((modifiers & Qt.ControlModifier) && formats.length > 1) {
            variants = formats;
            bookTitle = entry.title;
            open();
        } else {
            // Use the physical entry, including its own reading position.
            for (let i = 0; i < formats.length; ++i) {
                if (formats[i].filename === entry.filename) {
                    bookChosen(formats[i].entry);
                    return;
                }
            }
            bookChosen(entry);
        }
    }

    contentItem: ColumnLayout {
        QQC2.Label {
            text: root.bookTitle
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
        Repeater {
            model: root.variants
            delegate: QQC2.ItemDelegate {
                required property var modelData
                objectName: "chooseFormat" + modelData.format
                Layout.fillWidth: true
                contentItem: ColumnLayout {
                    QQC2.Label {
                        text: modelData.format
                        font.bold: true
                    }
                    QQC2.Label {
                        text: modelData.filename
                        textFormat: Text.PlainText
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                }
                onClicked: {
                    root.close();
                    root.bookChosen(modelData.entry);
                }
            }
        }
    }
}
