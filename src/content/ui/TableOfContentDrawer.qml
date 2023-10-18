// SPDX-FileCopyrightText: 2023 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls 2 as QQC2
import QtQuick.Layouts

import org.kde.kirigami 2 as Kirigami
import org.kde.kirigamiaddons.delegates 1 as Delegates
import org.kde.kirigamiaddons.treeview 1.0 as Tree
import org.kde.kitemmodels 1
import org.kde.arianna

Kirigami.OverlayDrawer {
    id: root

    property alias model: tableOfContentModel
    signal goTo(cfi: string)

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

            Kirigami.Heading {
                text: i18nc("@info:title", "Table of Contents")
            }
        }

        QQC2.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: treeView

                contentWidth: parent.availableWidth

                model: KDescendantsProxyModel {
                    model: TableOfContentModel {
                        id: tableOfContentModel
                    }
                }

                delegate: Delegates.RoundedTreeDelegate {
                    id: itemDelegate

                    required property string title
                    required property string href

                    text: title

                    onClicked: root.goTo(href)
                }
            }
        }
    }
}
