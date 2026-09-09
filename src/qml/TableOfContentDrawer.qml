// SPDX-FileCopyrightText: 2023 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls 2 as QQC2
import QtQuick.Layouts

import org.kde.kirigami 2 as Kirigami
import org.kde.kirigamiaddons.delegates 1 as Delegates
import org.kde.kirigamiaddons.treeview as Tree
import org.kde.kitemmodels 1
import org.kde.arianna

Kirigami.OverlayDrawer {
    id: root

    property alias model: tableOfContentModel
    property bool tableOfContentsModified: false
    property string currentTocId: ""
    property string currentHref: ""
    signal goTo(cfi: string)
    signal saveTableOfContents()

    function clearCurrentItem() {
        currentTocId = "";
        currentHref = "";
    }

    function stringValue(value) {
        return value === undefined || value === null ? "" : String(value);
    }

    function isCurrentItem(tocId, href) {
        const itemTocId = stringValue(tocId);
        const itemHref = stringValue(href);
        if (currentTocId.length > 0 && itemTocId.length > 0) {
            return itemTocId === currentTocId;
        }
        return currentHref.length > 0 && itemHref === currentHref;
    }

    function positionTreeViewAtIndex(index, tocId, href) {
        if (isCurrentItem(tocId, href) && treeView && index >= 0 && index < treeView.count) {
            treeView.positionViewAtIndex(index, ListView.Contain);
        }
    }

    function scheduleTreeViewPosition(index, tocId, href) {
        if (index >= 0) {
            Qt.callLater(root.positionTreeViewAtIndex, index, tocId, href);
        }
    }

    width: Kirigami.Units.gridUnit * 20
    edge: Qt.application.layoutDirection == Qt.RightToLeft ? Qt.LeftEdge : Qt.RightEdge
    handleClosedIcon.name: 'format-list-ordered'
    handleClosedToolTip: i18nc("@info:tooltip", "Open table of contents")
    handleOpenToolTip: i18nc("@info:tooltip", "Close table of contents")

    topPadding: 0
    leftPadding: 0
    rightPadding: 0

    Kirigami.Theme.colorSet: Kirigami.Theme.View

    contentItem: ColumnLayout {
        spacing: 0

        QQC2.ToolBar {
            Layout.fillWidth: true
            Layout.preferredHeight: applicationWindow().pageStack.globalToolBar.preferredHeight

            leftPadding: Kirigami.Units.largeSpacing
            rightPadding: Kirigami.Units.smallSpacing
            topPadding: Kirigami.Units.smallSpacing
            bottomPadding: Kirigami.Units.smallSpacing

            RowLayout {
                anchors.fill: parent

                Kirigami.Heading {
                    Layout.fillWidth: true
                    text: i18nc("@info:title", "Table of Contents")
                }

                QQC2.ToolButton {
                    icon.name: "document-save"
                    text: i18nc("@action:button", "Write Table of Contents to PDF")
                    enabled: root.tableOfContentsModified
                    display: QQC2.AbstractButton.IconOnly
                    onClicked: root.saveTableOfContents()
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }
            }
        }

        QQC2.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: treeView

                contentWidth: parent.availableWidth
                clip: true

                model: KDescendantsProxyModel {
                    model: TableOfContentModel {
                        id: tableOfContentModel
                    }
                }

                delegate: Delegates.RoundedTreeDelegate {
                    id: itemDelegate

                    required property var model
                    readonly property string title: root.stringValue(model.title)
                    readonly property string href: root.stringValue(model.href)
                    readonly property string tocId: root.stringValue(model.tocId)
                    readonly property bool currentTocItem: root.isCurrentItem(tocId, href)

                    index: model.index
                    kDescendantLevel: model.kDescendantLevel
                    kDescendantHasSiblings: model.kDescendantHasSiblings
                    kDescendantExpandable: model.kDescendantExpandable
                    kDescendantExpanded: model.kDescendantExpanded

                    text: title
                    highlighted: currentTocItem
                    font.bold: currentTocItem

                    onClicked: {
                        if (href.length > 0) {
                            root.goTo(href);
                        }
                    }

                    onCurrentTocItemChanged: if (currentTocItem && root.drawerOpen) {
                        root.scheduleTreeViewPosition(model.index, tocId, href);
                    }

                    Component.onCompleted: if (currentTocItem && root.drawerOpen) {
                        root.scheduleTreeViewPosition(model.index, tocId, href);
                    }
                }
            }
        }
    }
}
