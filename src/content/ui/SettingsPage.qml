import QtQuick 2.15
import QtQuick.Window 2.15
import QtQuick.Controls 2.15 as QQC2
import QtQuick.Dialogs 1.3 as Dialogs
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.13 as Kirigami
import org.kde.arianna 1.0

Kirigami.ScrollablePage {
    id: root

    title: i18n("Settings")

    Kirigami.FormLayout {
        Layout.fillWidth: true

        Kirigami.Separator {
            Kirigami.FormData.isSection: true

            Kirigami.FormData.label: i18n("Appearance")
        }

        QQC2.SpinBox {
            from: Kirigami.Units.smallSpacing
            to: Screen.desktopAvailableWidth

            Kirigami.FormData.label: i18n("Maximum width:")

            value: Config.maxWidth
            onValueModified: Config.maxWidth = value
        }

        QQC2.SpinBox {
            from: 0
            to: Screen.desktopAvailableWidth

            Kirigami.FormData.label: i18n("Margin:")

            value: Config.margin
            onValueModified: Config.margin = value
        }

        QQC2.Button {
            text: i18n("Change default font")
            Kirigami.FormData.label: i18n("Font:")

            onClicked: fontDialog.open()
        }
        QQC2.CheckBox {
            text: i18n("Use publisher font")

            checked: Config.usePublisherFont
            onCheckStateChanged: Config.usePublisherFont = checkState
        }

        QQC2.CheckBox {
            text: i18n("Justify text")
            Kirigami.FormData.label: i18n("Text flow:")

            checked: Config.justify
            onCheckStateChanged: Config.justify = checkState
        }
        QQC2.CheckBox {
            text: i18n("Hyphenate text")

            checked: Config.hyphenate
            onCheckStateChanged: Config.hyphenate = checkState
        }

        QQC2.SpinBox {
            from: 10
            to: 50
            textFromValue: (value, locale) => Number(value / 10).toLocaleString(locale, 'f', 1)
            valueFromText: (text, locale) => Number.fromLocaleString(locale, text) * 10

            Kirigami.FormData.label: i18n("Line height:")

            value: Config.spacing * 10
            onValueModified: Config.spacing = value / 10
        }

        QQC2.SpinBox {
            from: 0
            to: 100
            stepSize: 5
            textFromValue: (value, locale) => Number(value / 100).toLocaleString(locale, 'f', 2)
            valueFromText: (text, locale) => Number.fromLocaleString(locale, text) * 100

            Kirigami.FormData.label: i18n("Brightness:")

            value: Config.brightness * 100
            onValueModified: Config.brightness = value / 100

        }

        QQC2.CheckBox {
            text: i18n("Invert colors")
            Kirigami.FormData.label: i18n("Colors:")

            checked: Config.invert
            onCheckStateChanged: Config.invert = checkState
        }
    }

    Dialogs.FontDialog {
        id: fontDialog

        title: i18n("Change default font")

        font: Config.defaultFont

        onAccepted: Config.defaultFont = font;
        onRejected: font = Config.defaultFont;
    }
}
