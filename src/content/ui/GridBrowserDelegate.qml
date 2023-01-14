/*
   SPDX-FileCopyrightText: 2016 (c) Matthieu Gallien <matthieu_gallien@yahoo.fr>
   SPDX-FileCopyrightText: 2021 (c) Devin Lin <espidev@gmail.com>

   SPDX-License-Identifier: LGPL-3.0-or-later
 */

import QtQuick 2.15
import QtQuick.Controls 2.2
import QtQuick.Window 2.2
import QtQml.Models 2.1
import QtQuick.Layouts 1.2
import org.kde.kirigami 2.19 as Kirigami
import QtGraphicalEffects 1.15

FocusScope {
    id: gridEntry

    property url imageUrl
    property url fileUrl
    property string mainText
    property string secondaryText
    property bool delegateDisplaySecondaryText: true
    property bool isPartial
    property bool isSelected

    signal open()

    SystemPalette {
        id: myPalette
    }

    property color stateIndicatorColor: {
        if (gridEntry.activeFocus || hoverHandle.pressed || hoverHandle.containsMouse) {
            return Kirigami.Theme.highlightColor;
        } else if (gridEntry.isSelected && !Kirigami.Settings.isMobile) {
            return myPalette.mid;
        } else {
            return "transparent";
        }
    }
    property real stateIndicatorOpacity: {
        if ((!Kirigami.Settings.isMobile && gridEntry.activeFocus) ||
            (!Kirigami.Settings.isMobile && gridEntry.isSelected) || hoverHandle.pressed || hoverHandle.containsMouse) {
            return 0.3;
        } else {
            return 0;
        }
    }

    property bool delegateLoaded: true

    ListView.onPooled: delegateLoaded = false
    ListView.onReused: delegateLoaded = true

    // open mobile context menu
    function openContextMenu() {
        contextMenuLoader.active = true;
        contextMenuLoader.item.open();
    }

    Keys.onReturnPressed: open()
    Keys.onEnterPressed: open()

    Accessible.role: Accessible.ListItem
    Accessible.name: mainText

    Item {
        id: parentItem
        anchors.fill: parent
        // mobile uses more spacing between delegates
        anchors.margins: Kirigami.Settings.isMobile ? Kirigami.Units.largeSpacing : 0

        // highlight colour
        Rectangle {
            id: stateIndicator

            z: Kirigami.Settings.isMobile ? 1 : 0 // on desktop, we want hover actions to be above highlight

            anchors.fill: parent
            // expand margins of highlight box on mobile, so that it doesn't look like it's clipping the text
            anchors.leftMargin: Kirigami.Settings.isMobile ? -Kirigami.Units.smallSpacing : 0
            anchors.rightMargin: Kirigami.Settings.isMobile ? -Kirigami.Units.smallSpacing : 0
            anchors.bottomMargin: Kirigami.Settings.isMobile ? -Kirigami.Units.smallSpacing : 0
            color: stateIndicatorColor
            opacity: stateIndicatorOpacity
            radius: Kirigami.Settings.isMobile ? Kirigami.Units.smallSpacing : 3
        }

        // click handler
        MouseArea {
            id: hoverHandle

            anchors.fill: parent
            // fix mousearea from stealing swipes from flickable
            propagateComposedEvents: false
            onReleased: {
                if (!propagateComposedEvents) {
                    propagateComposedEvents = true
                }
            }

            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor

            onClicked: open()

            TextMetrics {
                id: mainLabelSize
                font: mainLabel.font
                text: mainLabel.text
            }

            Image {
                id: coverImage
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: Kirigami.Settings.isMobile ? 0 : Kirigami.Units.largeSpacing
                width: gridEntry.width - 2 * Kirigami.Units.largeSpacing
                height: gridEntry.width - 2 * Kirigami.Units.largeSpacing

                sourceSize.width: width
                sourceSize.height: height
                fillMode: Image.PreserveAspectFit

                source: imageUrl ? imageUrl : ""

                asynchronous: true

                layer.enabled: !Kirigami.Settings.isMobile // don't use drop shadow on mobile
                layer.effect: DropShadow {
                    source: coverImage
                    radius: 16
                    samples: (radius * 2) + 1
                    cached: true
                    color: myPalette.shadow
                }
            }

            // labels
            RowLayout {
                id: labels
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.top: coverImage.bottom

                ColumnLayout {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.fillWidth: true
                    spacing: 0

                    Kirigami.Heading {
                        id: mainLabel
                        text: gridEntry.mainText

                        level: Kirigami.Settings.isMobile ? 6 : 4

                        // FIXME: Center-aligned text looks better overall, but
                        // sometimes results in font kerning issues
                        // See https://bugreports.qt.io/browse/QTBUG-49646
                        horizontalAlignment: Kirigami.Settings.isMobile ? Text.AlignLeft : Text.AlignHCenter

                        Layout.fillWidth: true
                        Layout.maximumHeight: delegateDisplaySecondaryText ? mainLabelSize.boundingRect.height : mainLabelSize.boundingRect.height * 2
                        Layout.alignment: Kirigami.Settings.isMobile ? Qt.AlignLeft : Qt.AlignVCenter
                        Layout.leftMargin: Kirigami.Settings.isMobile ? 0 : Kirigami.Units.largeSpacing
                        Layout.rightMargin: Kirigami.Settings.isMobile ? 0 : Kirigami.Units.largeSpacing

                        wrapMode: !Kirigami.Settings.isMobile && delegateDisplaySecondaryText ? Label.NoWrap : Label.Wrap
                        maximumLineCount: Kirigami.Settings.isMobile ? 1 : 2
                        elide: Text.ElideRight
                    }

                    Label {
                        id: secondaryLabel
                        visible: delegateDisplaySecondaryText
                        text: gridEntry.secondaryText

                        opacity: 0.6

                        // FIXME: Center-aligned text looks better overall, but
                        // sometimes results in font kerning issues
                        // See https://bugreports.qt.io/browse/QTBUG-49646
                        horizontalAlignment: Kirigami.Settings.isMobile ? Text.AlignLeft : Text.AlignHCenter

                        Layout.fillWidth: true
                        Layout.alignment: Kirigami.Settings.isMobile ? Qt.AlignLeft : Qt.AlignVCenter
                        Layout.topMargin: Kirigami.Settings.isMobile ? Kirigami.Units.smallSpacing : 0
                        Layout.leftMargin: Kirigami.Settings.isMobile ? 0 : Kirigami.Units.largeSpacing
                        Layout.rightMargin: Kirigami.Settings.isMobile ? 0 : Kirigami.Units.largeSpacing

                        maximumLineCount: Kirigami.Settings.isMobile ? 1 : -1
                        elide: Text.ElideRight
                        font: Kirigami.Settings.isMobile ? Kirigami.Theme.smallFont : Kirigami.Theme.defaultFont
                    }
                }
            }
        }
    }
}
