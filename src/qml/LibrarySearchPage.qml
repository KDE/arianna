// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.arianna

Kirigami.Page {
    id: root
    objectName: "libraryFullTextSearchPage"

    required property LibrarySearchModel searchModel
    readonly property bool librarySearchPage: true
    signal openResult(string filename, string location, string query, int indexAtLocation)

    title: i18nc("@title:window, %1 is the number of books with matches", "Library Full-text Search(%1)", root.searchModel.bookCount)
    padding: Kirigami.Units.largeSpacing

    ListView {
        id: results
        objectName: "libraryFullTextSearchResults"
        anchors.fill: parent
        clip: true
        model: root.searchModel
        reuseItems: true
        boundsBehavior: Flickable.StopAtBounds
        QQC2.ScrollBar.vertical: QQC2.ScrollBar {}

        header: QQC2.Label {
            width: results.width
            bottomPadding: Kirigami.Units.largeSpacing
            text: i18nc("@info", "Search text: %1", root.searchModel.query)
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: Math.max(0, parent.width - Kirigami.Units.largeSpacing * 2)
            visible: results.count === 0
            text: root.searchModel.running ? i18nc("@info", "Searching…") : i18n("No search results")
        }

        delegate: QQC2.ItemDelegate {
            id: resultDelegate
            required property var result
            required property int index
            readonly property bool collapsible: result.heading && result.count >= 3
            objectName: result.heading ? "libraryFullTextSearchBook" : "libraryFullTextSearchHit"
            width: results.width
            text: result.heading ? result.title : result.excerpt
            Accessible.name: text
            Accessible.description: collapsible ? (result.collapsed ? i18nc("@action:button", "Show matches") : i18nc("@action:button", "Hide matches")) : ""
            hoverEnabled: collapsible || !result.heading
            QQC2.ToolTip.visible: collapsible && hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            QQC2.ToolTip.text: Accessible.description
            onClicked: {
                if (collapsible)
                    root.searchModel.toggleBook(index)
                else if (!result.heading)
                    root.openResult(result.filename, result.location, root.searchModel.query, result.indexAtLocation)
            }
            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing
                Item {
                    objectName: "libraryFullTextSearchSeparator"
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.largeSpacing
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                    Layout.preferredHeight: resultDelegate.result.separator === 2 ? 5 : 1
                    visible: resultDelegate.result.heading && resultDelegate.result.separator > 0
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        height: 1
                        color: Kirigami.Theme.disabledTextColor
                    }
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        visible: resultDelegate.result.separator === 2
                        color: Kirigami.Theme.disabledTextColor
                    }
                }
                RowLayout {
                    visible: resultDelegate.result.heading
                    Layout.fillWidth: true
                    Kirigami.Icon {
                        visible: resultDelegate.collapsible
                        source: resultDelegate.result.collapsed ? "arrow-right" : "arrow-down"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    }
                    Item {
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 2.5
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 3.5
                        Image {
                            id: cover
                            objectName: "libraryFullTextSearchCover"
                            anchors.fill: parent
                            source: resultDelegate.result.thumbnail || ""
                            sourceSize.width: 100
                            sourceSize.height: 140
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                        }
                        Kirigami.Icon {
                            anchors.fill: parent
                            source: "book"
                            visible: cover.status !== Image.Ready
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: resultDelegate.result.number + ". " + (resultDelegate.result.title || "")
                            textFormat: Text.PlainText
                            font.bold: true
                            wrapMode: Text.Wrap
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: resultDelegate.result.author || ""
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: i18ncp("@info", "%1 match", "%1 matches", resultDelegate.result.count || 0)
                            color: Kirigami.Theme.disabledTextColor
                        }
                    }
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    visible: resultDelegate.result.heading && (resultDelegate.result.error || "").length > 0
                    text: resultDelegate.result.error || ""
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    visible: !resultDelegate.result.heading
                    text: resultDelegate.result.number + ". " + (resultDelegate.result.section || "")
                    textFormat: Text.PlainText
                    color: Kirigami.Theme.disabledTextColor
                    wrapMode: Text.Wrap
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    visible: !resultDelegate.result.heading
                    text: resultDelegate.result.markup || ""
                    textFormat: Text.RichText
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
