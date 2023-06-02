// SPDX-FileCopyrightText: Šimon Rataj <ratajs@ratajs.cz>
// SPDX-License-Identifier: MIT

import QtQuick 2.15
import QtQuick.Window 2.15
import QtQuick.Controls 2.15 as QQC2
import QtQuick.Dialogs 1.3 as Dialogs
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.13 as Kirigami
import org.kde.kirigamiaddons.labs.mobileform 0.1 as MobileForm
import org.kde.arianna 1.0

Kirigami.ScrollablePage {
    id: root

    title: i18n("Settings")

    leftPadding: 0
    rightPadding: 0

    ColumnLayout {
        MobileForm.FormCard {
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            contentItem: ColumnLayout {
                spacing: 0

                MobileForm.FormCardHeader {
                    title: i18n("Appearance")
                }

                MobileForm.FormSpinBoxDelegate {
                    label: i18n("Maximum width:")

                    from: Kirigami.Units.smallSpacing
                    to: Screen.desktopAvailableWidth
                    value: Config.maxWidth
                    onValueChanged: {
                        Config.maxWidth = value;
                        Config.save();
                    }
                }

                MobileForm.FormDelegateSeparator {}

                MobileForm.FormSpinBoxDelegate {
                    label: i18n("Margin:")

                    from: 0
                    to: Screen.desktopAvailableWidth
                    value: Config.margin
                    onValueChanged: {
                        Config.margin = value;
                        Config.save();
                    }
                }

                MobileForm.FormDelegateSeparator {}

                MobileForm.FormCheckDelegate {
                    id: showProgress
                    text: i18n("Show progress")

                    checked: Config.showProgress
                    onCheckedChanged: {
                        Config.showProgress = checked;
                        Config.save();
                    }
                }
            }
        }

        MobileForm.FormCard {
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            contentItem: ColumnLayout {
                spacing: 0

                MobileForm.FormCardHeader {
                    title: i18n("Font")
                }

                MobileForm.FormButtonDelegate {
                    text: i18n("Change default font")
                    onClicked: fontDialog.open()
                }

                MobileForm.FormSwitchDelegate {
                    text: i18n("Use publisher font")

                    checked: Config.usePublisherFont
                    onCheckedChanged: {
                        Config.usePublisherFont = checked;
                        Config.save();
                    }
                }
            }
        }

        MobileForm.FormCard {
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            contentItem: ColumnLayout {
                spacing: 0

                MobileForm.FormCardHeader {
                    title: i18n("Text flow")
                }

                MobileForm.FormCheckDelegate {
                    id: justifyText
                    text: i18n("Justify text")

                    checked: Config.justify
                    onCheckedChanged: {
                        Config.justify = checked;
                        Config.save();
                    }
                }

                MobileForm.FormDelegateSeparator { above: justifyText; below: hyphenateText }

                MobileForm.FormCheckDelegate {
                    id: hyphenateText
                    text: i18n("Hyphenate text")

                    checked: Config.hyphenate
                    onCheckedChanged: {
                        Config.hyphenate = checked;
                        Config.save();
                    }
                }

                MobileForm.FormDelegateSeparator { above: hyphenateText }

                MobileForm.FormSpinBoxDelegate {
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

                MobileForm.FormDelegateSeparator { above: hyphenateText }

                MobileForm.FormSpinBoxDelegate {
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
        }

        MobileForm.FormCard {
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            contentItem: ColumnLayout {
                spacing: 0

                MobileForm.FormCardHeader {
                    title: i18n("Colors")
                }

                MobileForm.FormSwitchDelegate {
                    text: i18n("Invert colors")

                    checked: Config.invert
                    onCheckedChanged: {
                        Config.invert = checked
                        Config.save();
                    }
                }
            }
        }

        MobileForm.FormCard {
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            contentItem: ColumnLayout {
                spacing: 0
                Component {
                    id: aboutPage
                    MobileForm.AboutPage {
                        aboutData: About
                    }
                }
                Component {
                    id: aboutKDE
                    MobileForm.AboutKDE {}
                }

                MobileForm.FormButtonDelegate {
                    text: i18n("About Arianna")
                    onClicked: applicationWindow().pageStack.layers.push(aboutPage)
                }

                MobileForm.FormDelegateSeparator {}

                MobileForm.FormButtonDelegate {
                    text: i18n("About KDE")
                    onClicked: applicationWindow().pageStack.layers.push(aboutKDE)
                }
            }
        }
    }

    Dialogs.FontDialog {
        id: fontDialog

        title: i18n("Change default font")

        font: Config.defaultFont

        onAccepted: {
            Config.defaultFont = font;
            Config.save();
        }
        onRejected: font = Config.defaultFont;
    }
}
