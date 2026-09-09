/*
   SPDX-FileCopyrightText: 2016 (c) Matthieu Gallien <matthieu_gallien@yahoo.fr>
   SPDX-FileCopyrightText: 2021 (c) Devin Lin <espidev@gmail.com>

   SPDX-License-Identifier: LGPL-3.0-or-later
 */

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Window
import QtQml.Models
import QtQuick.Layouts
import QtQuick.Effects

import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.delegates as Delegates
import org.kde.quickcharts as Charts
import org.kde.arianna

Delegates.RoundedItemDelegate {
    id: gridEntry

    required property url imageUrl
    required property string iconName
    required property string mainText
    required property string secondaryText
    required property int currentProgress
    property string typeIconSource: ""
    property bool showPasswordBadge: false

    SystemPalette {
        id: myPalette
    }

    property color stateIndicatorColor: {
        if (gridEntry.activeFocus || gridEntry.pressed || gridEntry.hovered) {
            return Kirigami.Theme.highlightColor;
        } else {
            return "transparent";
        }
    }

    property real stateIndicatorOpacity: {
        if ((!Kirigami.Settings.isMobile && gridEntry.activeFocus) ||
            !Kirigami.Settings.isMobile || gridEntry.pressed || gridEntry.hovered) {
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

    Accessible.role: Accessible.ListItem
    Accessible.name: mainText

    contentItem: ColumnLayout {
        Item {
            id: coverArea

            readonly property real baseCoverMargin: Kirigami.Settings.isMobile ? 0 : Kirigami.Units.largeSpacing
            readonly property real paintedCoverWidth: coverImage.paintedWidth > 0 ? coverImage.paintedWidth : coverImage.width
            readonly property real paintedCoverHeight: coverImage.paintedHeight > 0 ? coverImage.paintedHeight : coverImage.height
            readonly property real paintedCoverLeft: coverImage.x + Math.max(0, (coverImage.width - paintedCoverWidth) / 2)
            readonly property real paintedCoverTop: coverImage.y + Math.max(0, (coverImage.height - paintedCoverHeight) / 2)
            readonly property real paintedCoverRight: paintedCoverLeft + paintedCoverWidth
            readonly property real coverBadgeSize: Math.max(Kirigami.Units.gridUnit * 0.75, paintedCoverWidth * 0.20)
            readonly property real coverBadgeInset: Math.max(1, coverBadgeSize * 0.04)

            Layout.fillWidth: true
            Layout.preferredHeight: gridEntry.width - 2 * Kirigami.Units.largeSpacing

            Image {
                id: coverImage

                height: width

                readonly property bool hasCoverSource: source.toString().length > 0

                fillMode: Image.PreserveAspectFit
                source: gridEntry.imageUrl.toString().length > 0 ? gridEntry.imageUrl : ''
                visible: hasCoverSource
                asynchronous: true
                cache: hasCoverSource

                sourceSize {
                    width: coverImage.hasCoverSource ? coverImage.width : 0
                    height: coverImage.hasCoverSource ? coverImage.height : 0
                }

                anchors {
                    top: parent.top
                    left: parent.left
                    right: parent.right
                    topMargin: coverArea.baseCoverMargin
                    leftMargin: coverArea.baseCoverMargin
                    rightMargin: coverArea.baseCoverMargin
                }
            }

            MultiEffect {
                source: coverImage
                readonly property bool hasVisibleCover: !Kirigami.Settings.isMobile && coverImage.hasCoverSource && coverImage.status === Image.Ready

                enabled: hasVisibleCover // don't use drop shadow on mobile
                visible: hasVisibleCover

                shadowColor: myPalette.shadow
                autoPaddingEnabled: true
                shadowBlur: 1.0
                shadowEnabled: true
                shadowVerticalOffset: 3
                shadowHorizontalOffset: 1

                anchors.fill: coverImage
            }

            Kirigami.Icon {
                id: fallBackIcon

                anchors {
                    fill: coverImage
                    margins: Kirigami.Settings.isMobile ? 0 : Kirigami.Units.largeSpacing
                }

                source: gridEntry.iconName
                visible: gridEntry.iconName.length > 0
            }

            Image {
                id: typeIcon

                readonly property bool pdfIcon: source.toString().indexOf("pdf_icon") !== -1
                readonly property real iconAspectRatio: pdfIcon ? 2 / 3 : 1
                readonly property real topPaddingCompensation: height * (pdfIcon ? 0.12 : 0.07)

                width: height * iconAspectRatio
                height: coverArea.coverBadgeSize
                x: coverArea.paintedCoverLeft + coverArea.coverBadgeInset
                y: coverArea.paintedCoverTop + coverArea.coverBadgeInset - topPaddingCompensation
                source: gridEntry.typeIconSource
                visible: source.toString().length > 0
                asynchronous: true
                fillMode: Image.PreserveAspectFit

                sourceSize {
                    width: Math.max(1, typeIcon.width * Screen.devicePixelRatio)
                    height: Math.max(1, typeIcon.height * Screen.devicePixelRatio)
                }
            }

            Charts.PieChart {
                id: chart

                width: coverArea.coverBadgeSize
                height: coverArea.coverBadgeSize
                x: coverArea.paintedCoverRight - width - coverArea.coverBadgeInset
                y: coverArea.paintedCoverTop + coverArea.coverBadgeInset

                filled: true

                visible: !gridEntry.showPasswordBadge && gridEntry.currentProgress !== 0 && gridEntry.currentProgress !== 100 && gridEntry.iconName === '' && Config.showProgress

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
                visible: !gridEntry.showPasswordBadge && gridEntry.currentProgress === 0 && gridEntry.iconName === ''
                x: coverArea.paintedCoverRight - width - coverArea.coverBadgeInset
                y: coverArea.paintedCoverTop + coverArea.coverBadgeInset

                text: i18nc("should be keep short, inside a label. Will be in uppercase", "New")
                color: "white"
                padding: Math.max(2, Math.round(coverArea.coverBadgeSize * 0.12))
                font.pixelSize: Math.max(1, Math.round(coverArea.coverBadgeSize * 0.42))

                background: Rectangle {
                    color: Kirigami.Theme.highlightColor
                    radius: height
                }
            }

            Rectangle {
                id: passwordBadge

                width: coverArea.coverBadgeSize
                height: coverArea.coverBadgeSize
                x: coverArea.paintedCoverRight - width - coverArea.coverBadgeInset
                y: coverArea.paintedCoverTop + coverArea.coverBadgeInset
                radius: width / 2
                color: Kirigami.Theme.alternateBackgroundColor
                border.color: Kirigami.ColorUtils.linearInterpolation(Kirigami.Theme.textColor, Kirigami.Theme.backgroundColor, 0.45)
                border.width: 1
                visible: gridEntry.showPasswordBadge

                Kirigami.Icon {
                    anchors.fill: parent
                    anchors.margins: Math.max(2, Math.round(parent.width * 0.22))
                    source: "object-locked-symbolic"
                }
            }
        }

        // labels
        RowLayout {
            id: labels

            Layout.fillWidth: true

            TextMetrics {
                id: mainLabelSize
                font: mainLabel.font
                text: mainLabel.text
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
