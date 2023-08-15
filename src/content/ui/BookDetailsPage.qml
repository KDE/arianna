// SPDX-FileCopyrightText: Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick 2.15
import QtQuick.Controls 2.15 as QQC2
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.13 as Kirigami
import org.kde.kirigamiaddons.labs.mobileform 0.1 as MobileForm
import org.kde.arianna 1.0

Kirigami.ScrollablePage {
    id: root

    property var metadata

    title: i18n("Book Details")

    leftPadding: 0
    rightPadding: 0

    ColumnLayout {
        MobileForm.FormCard {
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            contentItem: ColumnLayout {
                spacing: 0

                MobileForm.FormCardHeader {
                    title: root.metadata.title
                }

                MobileForm.FormTextDelegate {
                    text: i18n("Author:")
                    description: root.metadata.creator
                    visible: description.length > 0
                }

                MobileForm.FormDelegateSeparator {}

                MobileForm.FormTextDelegate {
                    text: i18n("Description:")
                    description: root.metadata.description
                    visible: description.length > 0
                }

                MobileForm.FormDelegateSeparator {}

                MobileForm.FormTextDelegate {
                    text: i18n("Publisher:")
                    description: root.metadata.publisher
                    visible: description.length > 0
                }

                MobileForm.FormDelegateSeparator {}

                MobileForm.FormTextDelegate {
                    text: i18n("Language:")
                    description: Qt.locale(root.metadata.language).nativeLanguageName
                    visible: description.length > 0
                }

                MobileForm.FormDelegateSeparator {}

                MobileForm.FormTextDelegate {
                    text: i18n("Publishing date:")
                    description: /^\d+$/.test(root.metadata.pubdate) ? root.metadata.pubdate : new Date(root.metadata.pubdate).toLocaleDateString()
                    visible: description.length > 0
                }

                MobileForm.FormDelegateSeparator {}

                MobileForm.FormTextDelegate {
                    text: i18n("Copyright:")
                    description: root.metadata.rights
                    visible: description.length > 0
                }
            }
        }
    }
}