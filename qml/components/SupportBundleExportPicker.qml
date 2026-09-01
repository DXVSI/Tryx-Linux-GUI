pragma ComponentBehavior: Bound

import Qt.labs.folderlistmodel
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    objectName: "supportBundleFolderPicker"

    required property var controller
    required property url homeFolder
    property Item focusReturnItem: null
    property url currentFolder: homeFolder
    readonly property bool currentFolderReady:
        folderModel.status === FolderListModel.Ready

    function openForExport() {
        if (String(currentFolder).length === 0)
            currentFolder = homeFolder
        open()
    }

    function navigate(folderUrl) {
        currentFolder = folderUrl
    }

    function saveHere() {
        if (controller.busy || !currentFolderReady)
            return
        const folder = currentFolder
        close()
        Qt.callLater(() => root.controller.exportToFolder(folder))
    }

    parent: Overlay.overlay
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    width: parent
           ? Math.min(760, Math.max(520, parent.width - 48))
           : 760
    height: parent
            ? Math.min(650, Math.max(460, parent.height - 48))
            : 650
    modal: true
    focus: true
    padding: 20
    closePolicy: Popup.CloseOnEscape

    onOpened: homeButton.forceActiveFocus()
    onClosed: {
        if (focusReturnItem && focusReturnItem.forceActiveFocus)
            focusReturnItem.forceActiveFocus()
    }

    background: Rectangle {
        radius: 12
        color: "#1b1f23"
        border.width: 1
        border.color: "#4a535a"
    }

    FolderListModel {
        id: folderModel

        folder: root.currentFolder
        showFiles: false
        showDirs: true
        showDirsFirst: true
        showDotAndDotDot: false
        showHidden: hiddenFolders.checked
        showOnlyReadable: true
        sortField: FolderListModel.Name
        sortCaseSensitive: false
    }

    contentItem: ColumnLayout {
        objectName: "supportBundleDialogContent"
        spacing: 14

        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Export support report")
        Accessible.description:
            qsTr("Choose a local folder for the redacted JSON report.")

        Keys.onEscapePressed: event => {
            root.close()
            event.accepted = true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Export support report")
                    color: "#f4f6f7"
                    font.pixelSize: 20
                    font.bold: true
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Choose a local folder. The file name is generated automatically.")
                    color: "#9da5ac"
                    elide: Text.ElideRight
                }
            }

            ToolButton {
                objectName: "supportBundleFolderCloseButton"
                text: "×"
                Accessible.name: qsTr("Close")
                onClicked: root.close()
            }
        }

        Frame {
            Layout.fillWidth: true
            Layout.preferredHeight: 48

            background: Rectangle {
                radius: 7
                color: "#15181b"
                border.width: 1
                border.color: "#363d43"
            }

            Label {
                objectName: "supportBundleFolderPath"
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                text: String(root.currentFolder)
                textFormat: Text.PlainText
                color: "#c7cdd2"
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideMiddle
                Accessible.name: text
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Button {
                id: homeButton
                text: qsTr("Home")
                activeFocusOnTab: true
                onClicked: root.navigate(root.homeFolder)
            }

            Button {
                text: qsTr("Up")
                activeFocusOnTab: true
                enabled: String(folderModel.parentFolder).length > 0 &&
                         String(folderModel.parentFolder) !==
                         String(root.currentFolder)
                onClicked: root.navigate(folderModel.parentFolder)
            }

            Item { Layout.fillWidth: true }

            CheckBox {
                id: hiddenFolders
                text: qsTr("Show hidden folders")
                activeFocusOnTab: true
            }
        }

        Frame {
            Layout.fillWidth: true
            Layout.fillHeight: true

            background: Rectangle {
                radius: 8
                color: "#15181b"
                border.width: 1
                border.color: "#363d43"
            }

            ListView {
                objectName: "supportBundleFolderList"
                anchors.fill: parent
                anchors.margins: 4
                clip: true
                model: folderModel
                spacing: 2
                activeFocusOnTab: true
                Accessible.name: qsTr("Folders")

                ScrollBar.vertical: ScrollBar {}

                delegate: ItemDelegate {
                    id: folderDelegate

                    required property string fileName
                    required property url fileUrl
                    required property bool fileIsDir

                    width: ListView.view.width
                    height: 52
                    visible: fileIsDir
                    activeFocusOnTab: true
                    Accessible.name: fileName
                    Accessible.description: qsTr("Open folder")

                    onClicked:
                        root.navigate(folderDelegate.fileUrl)
                    Keys.onReturnPressed: event => {
                        root.navigate(folderDelegate.fileUrl)
                        event.accepted = true
                    }
                    Keys.onEnterPressed: event => {
                        root.navigate(folderDelegate.fileUrl)
                        event.accepted = true
                    }

                    contentItem: RowLayout {
                        spacing: 12

                        Rectangle {
                            Layout.preferredWidth: 54
                            Layout.preferredHeight: 28
                            radius: 5
                            color: "#30372d"
                            border.width: 1
                            border.color: "#7b883c"

                            Label {
                                anchors.centerIn: parent
                                text: qsTr("DIR")
                                color: "#def750"
                                font.pixelSize: 9
                                font.bold: true
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: folderDelegate.fileName
                            color: "#eef1f3"
                            elide: Text.ElideMiddle
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: folderModel.status === FolderListModel.Null
                text: qsTr("This folder cannot be opened")
                color: "#efb85f"
            }

            Label {
                anchors.centerIn: parent
                visible: folderModel.status === FolderListModel.Loading
                text: qsTr("Loading folder…")
                color: "#9da5ac"
            }

            Label {
                anchors.centerIn: parent
                visible: folderModel.status === FolderListModel.Ready &&
                         folderModel.count === 0
                text: qsTr("This folder has no subfolders")
                color: "#9da5ac"
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("The report stays on this computer until you share it manually. Existing files are never replaced.")
            color: "#8f989f"
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("Cancel")
                activeFocusOnTab: true
                onClicked: root.close()
            }

            PrimaryButton {
                objectName: "supportBundleFolderSaveButton"
                Layout.minimumHeight: 44
                text: qsTr("Save here")
                enabled: !root.controller.busy &&
                         root.currentFolderReady
                activeFocusOnTab: true
                Accessible.name: text
                Accessible.description:
                    qsTr("Create a new redacted JSON report in the selected folder")
                onClicked: root.saveHere()
            }
        }
    }
}
