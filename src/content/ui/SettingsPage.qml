// SPDX-FileCopyrightText: Šimon Rataj <ratajs@ratajs.cz>
// SPDX-License-Identifier: MIT

import QtQuick 2.15
import QtQuick.Window 2.15
import QtQuick.Controls 2.15 as QQC2
import QtQuick.Dialogs @QTQUICKDIALOG_VERSION@ as Dialogs
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.13 as Kirigami
import org.kde.kirigamiaddons.formcard 1.0 as FormCard
import org.kde.arianna 1.0

Kirigami.ScrollablePage {
    id: root

    title: i18n("Settings")

    leftPadding: 0
    rightPadding: 0

    ColumnLayout {
        FormCard.FormCard {
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
           

            FormCard.FormHeader {
                title: i18n("Appearance")
            }

            FormCard.FormSpinBoxDelegate {
                label: i18n("Maximum width:")

                from: Kirigami.Units.smallSpacing
                to: Screen.desktopAvailableWidth
                value: Config.maxWidth
                onValueChanged: {
                    Config.maxWidth = value;
                    Config.save();
                }
            }

            FormCard.FormDelegateSeparator {}

            FormCard.FormSpinBoxDelegate {
                label: i18n("Margin:")

                from: 0
                to: Screen.desktopAvailableWidth
                value: Config.margin
                onValueChanged: {
                    Config.margin = value;
                    Config.save();
                }
            }

            FormCard.FormDelegateSeparator {}

            FormCard.FormCheckDelegate {
                id: showProgress
                text: i18n("Show progress")

                checked: Config.showProgress
                onCheckedChanged: {
                    Config.showProgress = checked;
                    Config.save();
                }
            }
        }

        FormCard.FormCard {
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true

            FormCard.FormHeader {
                title: i18n("Font")
            }

            FormCard.FormButtonDelegate {
                text: i18n("Change default font")
                onClicked: fontDialog.open()
            }

            FormCard.FormSwitchDelegate {
                text: i18n("Use publisher font")

                checked: Config.usePublisherFont
                onCheckedChanged: {
                    Config.usePublisherFont = checked;
                    Config.save();
                }
            }
        }

        FormCard.FormCard {
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true

            FormCard.FormHeader {
                title: i18n("Text flow")
            }

            FormCard.FormCheckDelegate {
                id: justifyText
                text: i18n("Justify text")

                checked: Config.justify
                onCheckedChanged: {
                    Config.justify = checked;
                    Config.save();
                }
            }

            FormCard.FormDelegateSeparator { above: justifyText; below: hyphenateText }

            FormCard.FormCheckDelegate {
                id: hyphenateText
                text: i18n("Hyphenate text")

                checked: Config.hyphenate
                onCheckedChanged: {
                    Config.hyphenate = checked;
                    Config.save();
                }
            }

            FormCard.FormDelegateSeparator { above: hyphenateText }

            FormCard.FormSpinBoxDelegate {
                label: i18n("Line height:")

                from: 10
                to: 50
                textFromValue: (value, locale) => Number(value / 10).toLocaleString(locale, 'f', 1)
                valueFromText: (text, locale) => Number.fromLocaleString(locale, text) * 10

                value: Config.spacing * 10
                onValueChanged: {
                    Config.spacing = value / 10
                    Config.save();
                }
            }

            FormCard.FormDelegateSeparator { above: hyphenateText }

            FormCard.FormSpinBoxDelegate {
                label: i18n("Brightness:")

                from: 0
                to: 100
                stepSize: 5
                textFromValue: (value, locale) => Number(value / 100).toLocaleString(locale, 'f', 2)
                valueFromText: (text, locale) => Number.fromLocaleString(locale, text) * 100


                value: Config.brightness * 100
                onValueChanged: {
                    Config.brightness = value / 100
                    Config.save();
                }
            }
        }

        FormCard.FormCard {
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true

            FormCard.FormHeader {
                title: i18n("Colors")
            }

            FormCard.FormSwitchDelegate {
                text: i18n("Invert colors")

                checked: Config.invert
                onCheckedChanged: {
                    Config.invert = checked
                    Config.save();
                }
            }
        }

        FormCard.FormCard {
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true

            FormCard.FormButtonDelegate {
                Component {
                    id: aboutPage
                    FormCard.AboutPage {
                        aboutData: About
                    }
                }

                text: i18n("About Arianna")
                onClicked: applicationWindow().pageStack.layers.push(aboutPage)
            }

            FormCard.FormDelegateSeparator {}

            FormCard.FormButtonDelegate {
                Component {
                    id: aboutKDE
                    FormCard.AboutKDE {}
                }

                text: i18n("About KDE")
                onClicked: applicationWindow().pageStack.layers.push(aboutKDE)
            }
        }
    }

    Dialogs.FontDialog {
        id: fontDialog

        title: i18n("Change default font")

        @DIALOG_FONTDIALOG_FONT@: Config.defaultFont

        onAccepted: {
            Config.defaultFont = font;
            Config.save();
        }
        onRejected: font = Config.defaultFont;
    }
}
