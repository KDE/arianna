/*
   SPDX-FileCopyrightText: 2016 (c) Matthieu Gallien <matthieu_gallien@yahoo.fr>
   SPDX-FileCopyrightText: 2021 (c) Devin Lin <espidev@gmail.com>

   SPDX-License-Identifier: LGPL-3.0-or-later
 */

import QtQuick 2.15
import QtQuick.Controls 2.15 as QQC2
import QtQuick.Window 2.2
import QtQml.Models 2.1
import QtQuick.Layouts 1.2
import QtGraphicalEffects 1.15
import org.kde.kirigami 2.19 as Kirigami
import org.kde.quickcharts 1.0 as Charts

QQC2.ItemDelegate {
    id: gridEntry

    required property url imageUrl
    required property string iconName
    required property string mainText
    required property string secondaryText
    required property int currentProgress

    signal open()

    background: null

    SystemPalette {
        id: myPalette
    }

    property color stateIndicatorColor: {
        if (gridEntry.activeFocus || hoverHandle.pressed || hoverHandle.containsMouse) {
            return Kirigami.Theme.highlightColor;
        } else {
            return "transparent";
        }
    }

    property real stateIndicatorOpacity: {
        if ((!Kirigami.Settings.isMobile && gridEntry.activeFocus) ||
            !Kirigami.Settings.isMobile || hoverHandle.pressed || hoverHandle.containsMouse) {
            return 0.3;
        } else {
            return 0;
        }
    }

    // open mobile context menu
    function openContextMenu() {
        contextMenuLoader.active = true;
        contextMenuLoader.item.open();
    }

    Keys.onReturnPressed: open()
    Keys.onEnterPressed: open()
    Keys.onSpacePressed: open()

    Accessible.role: Accessible.ListItem
    Accessible.name: mainText

    Item {
        id: parentItem
        anchors {
            fill: parent
            // mobile uses more spacing between delegates
            margins: Kirigami.Settings.isMobile ? Kirigami.Units.largeSpacing : 0
            bottomMargin: Kirigami.Settings.isMobile ? Kirigami.Units.largeSpacing : Kirigami.Units.smallSpacing
        }

        // highlight colour
        Rectangle {
            id: stateIndicator

            z: Kirigami.Settings.isMobile ? 1 : 0 // on desktop, we want hover actions to be above highlight

            color: stateIndicatorColor
            opacity: stateIndicatorOpacity
            radius: Kirigami.Settings.isMobile ? Kirigami.Units.smallSpacing : 3

            // expand margins of highlight box on mobile, so that it doesn't look like
            // it's clipping the text
            anchors {
                fill: parent
                leftMargin: Kirigami.Settings.isMobile ? -Kirigami.Units.smallSpacing : 0
                rightMargin: Kirigami.Settings.isMobile ? -Kirigami.Units.smallSpacing : 0
                bottomMargin: Kirigami.Settings.isMobile ? -Kirigami.Units.smallSpacing : 0
            }
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

                width: gridEntry.width - 2 * Kirigami.Units.largeSpacing
                height: gridEntry.width - 2 * Kirigami.Units.largeSpacing

                fillMode: Image.PreserveAspectFit
                source: gridEntry.imageUrl != 'file://' ? gridEntry.imageUrl : ''
                asynchronous: true

                sourceSize {
                    width: width
                    height: height
                }

                anchors {
                    top: parent.top
                    left: parent.left
                    right: parent.right
                    margins: Kirigami.Settings.isMobile ? 0 : Kirigami.Units.largeSpacing
                }

                layer {
                    enabled: !Kirigami.Settings.isMobile // don't use drop shadow on mobile
                    effect: DropShadow {
                        source: coverImage
                        radius: 16
                        samples: (radius * 2) + 1
                        cached: true
                        color: myPalette.shadow
                    }
                }
            }

            Kirigami.Icon {
                id: fallBackIcon

                anchors {
                    fill: coverImage
                    margins: Kirigami.Settings.isMobile ? 0 : Kirigami.Units.largeSpacing
                }

                source: gridEntry.iconName
                visible: source !== undefined
            }

            Charts.PieChart {
                id: chart

                width: Kirigami.Units.gridUnit
                height: Kirigami.Units.gridUnit

                filled: true

                visible: gridEntry.currentProgress !== 0 && gridEntry.currentProgress !== 100 && gridEntry.iconName === ''

                anchors {
                    right: coverImage.right
                    top: coverImage.top
                }

                range {
                    from: 0
                    to: 100
                    automatic: false
                }

                valueSources: Charts.SingleValueSource {
                    value: gridEntry.currentProgress
                }

                colorSource: Charts.SingleValueSource {
                    value: Kirigami.Theme.highlightColor
                }
            }

            QQC2.Label {
                visible: gridEntry.currentProgress === 0 && gridEntry.iconName === ''

                text: i18nc("should be keep short, inside a label. Will be in uppercase", "New")
                color: "white"
                padding: 3

                background: Rectangle {
                    color: Kirigami.Theme.highlightColor
                    radius: height
                }

                anchors {
                    right: coverImage.right
                    top: coverImage.top
                }
            }

            // labels
            RowLayout {
                id: labels

                anchors {
                    left: parent.left
                    right: parent.right
                    bottom: parent.bottom
                    top: coverImage.bottom
                }

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
                        Layout.maximumHeight: mainLabelSize.boundingRect.height
                        Layout.alignment: Kirigami.Settings.isMobile ? Qt.AlignLeft : Qt.AlignVCenter
                        Layout.leftMargin: Kirigami.Settings.isMobile ? 0 : Kirigami.Units.largeSpacing
                        Layout.rightMargin: Kirigami.Settings.isMobile ? 0 : Kirigami.Units.largeSpacing

                        wrapMode: !Kirigami.Settings.isMobile && QQC2.Label.NoWrap
                        maximumLineCount: Kirigami.Settings.isMobile ? 1 : 2
                        elide: Text.ElideRight
                    }

                    QQC2.Label {
                        id: secondaryLabel

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
