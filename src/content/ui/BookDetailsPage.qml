// SPDX-FileCopyrightText: Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import org.kde.arianna as Arianna

FormCard.FormCardPage {
    id: root

    property var metadata

    title: i18nc("@info:title", "Book Details")

    FormCard.FormHeader {
        title: root.metadata.title
    }

    FormCard.FormCard {
        FormCard.FormTextDelegate {
            id: authorField

            text: i18n("Author:")
            description: root.metadata.author
            visible: description.length > 0
        }

        FormCard.FormDelegateSeparator {
            visible: authorField.visible
        }

        FormCard.FormTextDelegate {
            id: descriptionField

            text: i18n("Description:")
            description: root.metadata.description
            visible: description.length > 0
        }

        FormCard.FormDelegateSeparator {
            visible: descriptionField.visible
        }

        FormCard.FormTextDelegate {
            id: publisherField

            text: i18n("Publisher:")
            description: root.metadata.publisher
            visible: description.length > 0
        }

        FormCard.FormDelegateSeparator {
            visible: publisherField.visible
        }

        FormCard.FormTextDelegate {
            id: languageField

            text: i18n("Language:")
            description: Qt.locale(root.metadata.language).nativeLanguageName
            visible: description.length > 0
        }

        FormCard.FormDelegateSeparator {
            visible: languageField.visible
        }

        FormCard.FormTextDelegate {
            id: publishingField

            text: i18n("Publishing date:")
            description: /^\d+$/.test(root.metadata.pubdate) ? root.metadata.pubdate : new Date(root.metadata.pubdate).toLocaleDateString()
            visible: description.length > 0
        }

        FormCard.FormDelegateSeparator {
            visible: publishingField.visible && copyrightField.visible
        }

        FormCard.FormTextDelegate {
            id: copyrightField
            text: i18n("Copyright:")
            description: root.metadata.rights
            visible: description.length > 0
        }
    }
}
