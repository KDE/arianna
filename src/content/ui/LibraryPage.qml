// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick 2.15
import QtQuick.Controls 2.15 as QQC2
import QtWebEngine 1.4
import QtWebChannel 1.4
import QtQuick.Layouts 1.15
import Qt.labs.platform 1.1
import org.kde.kirigami 2.13 as Kirigami
import org.kde.arianna 1.0

Kirigami.ScrollablePage {
    title: i18n("Library")

    actions.main: Kirigami.Action {
        text: i18nc("@action:button", "Add Book")
        onTriggered: {
            const fileDialog = openFileDialog.createObject(QQC2.ApplicationWindow.overlay)
            fileDialog.accepted.connect(() => {
                const file = fileDialog.file;
                if (!file) {
                    return;
                }
                contentList.contentModel.addFile(file)
            })
            fileDialog.open();
        }
    }

    Component {
        id: openFileDialog

        FileDialog {
            id: root
            title: i18n("Please choose a file")
        }
    }

    ListView {
        model: BookListModel {
            id: contentList;
            contentModel: ContentList {
                autoSearch: false

                onSearchStarted: applicationWindow().isLoading = true
                onSearchCompleted: applicationWindow().isLoading = false

                ContentQuery {
                    type: ContentQuery.Epub
                    locations: Config.bookLocations
                }
            }
            onCacheLoadedChanged: {
                if(!cacheLoaded) {
                    return;
                }
                contentList.contentModel.setKnownFiles(contentList.knownBookFiles());
                contentList.contentModel.startSearch()
            }
        }

        delegate: Kirigami.BasicListItem {
            text: model.title
            subtitle: model.author.join(', ')
            onClicked: {
                applicationWindow().pageStack.layers.push('./EpubViewerPage.qml', {
                    url: 'file://' + model.filename,
                })
            }
        }
    }
}
