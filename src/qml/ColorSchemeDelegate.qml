// SPDX-FileCopyrightText: 2020 Carl Schwan <carlschwan@kde.org>
// SPDX-License-Identifier: LGPL-2.0-or-later

import QtQuick
import QtQuick.Layouts

import org.kde.kirigamiaddons.formcard as FormCard
import org.kde.arianna

FormCard.FormComboBoxDelegate {
    id: root

    text: i18n("Color theme")
    textRole: "display"
    valueRole: "display"
    model: ColorSchemer.model
    Component.onCompleted: {
        currentIndex = ColorSchemer.indexForCurrentScheme()
    }
    onCurrentValueChanged: {
        ColorSchemer.apply(currentIndex)
    }
}
